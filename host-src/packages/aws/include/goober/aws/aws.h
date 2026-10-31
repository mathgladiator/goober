#pragma once

// Amazon Web Services, as much as goober needs (design/machine/018.Mail.md, 019.Backups.md):
// Signature Version 4, S3 objects (put, get, list, delete), and SES v2 mail. Requests go
// through curl, like every other call the host makes; the signature and any session token
// travel in a private curl config file, never on its command line. S3-compatible services
// (Backblaze B2, Cloudflare R2, MinIO) work through `endpoint`.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goober::aws {

struct Credentials {
  std::string access_key_id;
  std::string secret_access_key;
  std::string session_token;  // only for temporary credentials
};

// ---- Signature Version 4 ----

using Pairs = std::vector<std::pair<std::string, std::string>>;

struct SignInput {
  std::string method;          // GET, PUT, POST, DELETE
  std::string host;            // the Host header exactly as sent: name, plus :port when not the default
  std::string path;            // the URI-encoded path, as sent (S3 keys encoded once, slashes kept)
  Pairs query;                 // decoded names and values; encoded and sorted when signed
  Pairs headers;               // more headers to sign, like content-type
  std::string payload_sha256;  // hex SHA-256 of the body, or "UNSIGNED-PAYLOAD"
  bool content_sha256_header = true;  // send and sign x-amz-content-sha256 (S3 requires it)
};

// RFC 3986 unreserved characters stay; everything else is %XX in uppercase hex. Slashes stay
// when encode_slash is false (an S3 key's path).
std::string uri_encode(std::string_view text, bool encode_slash);

// 20150830T123600Z, for x-amz-date.
std::string amz_date(int64_t unix_ms);

// The pieces of a signature, exposed for tests against AWS's published examples.
std::string canonical_request(const SignInput& in, std::string_view amz_date, const Credentials& creds);
std::string string_to_sign(std::string_view canonical, std::string_view amz_date, std::string_view region,
                           std::string_view service);
std::array<uint8_t, 32> signing_key(std::string_view secret, std::string_view date, std::string_view region,
                                    std::string_view service);

// The headers to send with the request: authorization, x-amz-date, and when they apply
// x-amz-content-sha256 and x-amz-security-token. Header names are lowercase.
Pairs sign(const SignInput& in, const Credentials& creds, std::string_view region, std::string_view service,
           std::string_view amz_date);

// ---- Requests ----

struct Endpoint {
  std::string scheme = "https";
  std::string host;  // with :port when not the scheme's default
};

struct Request {
  std::string service;  // "s3", "ses"
  std::string region;
  Endpoint endpoint;
  std::string method = "GET";
  std::string path = "/";  // URI-encoded
  Pairs query;
  Pairs headers;  // signed and sent, like content-type
  std::string body;
  std::filesystem::path upload;    // sent as the body instead of `body`, read in pieces
  std::filesystem::path download;  // a 2xx body is written here instead of returned
  int64_t timeout_ms = 30000;
};

struct Reply {
  int status = 0;     // 0 when nothing came back
  std::string body;   // the body (an error body even with `download`), at most 4 MiB
  std::string error;  // why nothing came back, or the service's error code and message
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Sends a signed request. `scratch` holds the private curl config while curl runs. http is
// allowed only to a loopback address (a stand-in service in tests and on a devbox).
Reply perform(const Request& req, const Credentials& creds, const std::filesystem::path& scratch);

// ---- S3 ----

struct S3Location {
  std::string bucket;
  std::string region = "us-east-1";
  std::string endpoint;  // "" for AWS; else like https://s3.example.com or http://127.0.0.1:9000
  std::string prefix;    // put before every key, like "goober/"
};

struct S3Object {
  std::string key;  // without the location's prefix
  int64_t size = 0;
  std::string last_modified;  // as S3 gives it, RFC 3339
};

using CredentialsFn = std::function<std::optional<Credentials>()>;

class S3 {
 public:
  S3(S3Location location, CredentialsFn credentials, std::filesystem::path scratch);
  const S3Location& location() const { return location_; }
  // A single PUT (S3 takes up to 5 GiB this way); the body's SHA-256 is signed.
  bool put_file(const std::string& key, const std::filesystem::path& file, std::string* error);
  bool put(const std::string& key, std::string_view bytes, std::string* error);
  bool get_file(const std::string& key, const std::filesystem::path& file, std::string* error);
  std::optional<std::string> get(const std::string& key, std::string* error);
  // Every object under the prefix (after the location's own), across as many pages as it takes.
  std::optional<std::vector<S3Object>> list(const std::string& prefix, std::string* error);
  bool remove(const std::string& key, std::string* error);

 private:
  Request base_request(const std::string& method, const std::optional<std::string>& key) const;
  Reply send(Request req, std::string* error);
  S3Location location_;
  CredentialsFn credentials_;
  std::filesystem::path scratch_;
};

// ---- SES ----

struct SesMessage {
  std::string from, to;
  std::string raw;  // the whole MIME message
  std::string configuration_set;
};

struct SesResult {
  bool sent = false;
  bool permanent = false;  // a refusal asking again won't change (a 4xx other than 403, 408, or 429)
  std::string message_id;
  std::string error;
  int status = 0;
};

// SES v2 SendEmail with raw content. `endpoint` overrides email.$region.amazonaws.com (tests).
SesResult ses_send(const SesMessage& message, const Credentials& creds, const std::string& region,
                   const std::filesystem::path& scratch, const std::string& endpoint = "");

// SES v2 GetEmailIdentity: whether a domain or address may send, and its DKIM (Easy DKIM's three
// CNAME tokens and their status). `exists` is false when SES doesn't know the identity.
struct SesIdentity {
  bool ok = false;      // SES answered (exists or not)
  bool exists = false;
  bool verified_for_sending = false;
  std::string verification_status;  // SUCCESS, PENDING, FAILED, TEMPORARY_FAILURE, NOT_STARTED
  std::string dkim_status;          // the same vocabulary
  bool dkim_signing = false;
  std::vector<std::string> dkim_tokens;
  std::string error;
};
SesIdentity ses_identity(const std::string& identity, const Credentials& creds, const std::string& region,
                         const std::filesystem::path& scratch, const std::string& endpoint = "");

}  // namespace goober::aws

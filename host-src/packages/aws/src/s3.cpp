// S3 objects: put, get, list (ListObjectsV2), and delete.

#include <algorithm>

#include "goober/aws/aws.h"
#include "goober/base/encoding.h"

namespace goober::aws {
namespace fs = std::filesystem;

namespace {

std::string between(std::string_view text, std::string_view open, std::string_view close, size_t* from = nullptr) {
  size_t start = from ? *from : 0;
  auto a = text.find(open, start);
  if (a == std::string_view::npos) return "";
  a += open.size();
  auto b = text.find(close, a);
  if (b == std::string_view::npos) return "";
  if (from) *from = b + close.size();
  return std::string(text.substr(a, b - a));
}

// ListObjectsV2 with encoding-type=url gives keys form-encoded.
std::string url_decoded(std::string text) {
  std::replace(text.begin(), text.end(), '+', ' ');
  return base::percent_decode(text).value_or(text);
}

}  // namespace

S3::S3(S3Location location, CredentialsFn credentials, fs::path scratch)
    : location_(std::move(location)), credentials_(std::move(credentials)), scratch_(std::move(scratch)) {}

Request S3::base_request(const std::string& method, const std::optional<std::string>& key) const {
  Request r;
  r.service = "s3";
  r.region = location_.region;
  r.method = method;
  bool path_style = !location_.endpoint.empty() || location_.bucket.find('.') != std::string::npos;
  if (location_.endpoint.empty()) {
    // Virtual-hosted names unless the bucket has a dot, which TLS wildcard names can't cover.
    r.endpoint.host = path_style ? "s3." + location_.region + ".amazonaws.com"
                                 : location_.bucket + ".s3." + location_.region + ".amazonaws.com";
  } else {
    std::string e = location_.endpoint;
    auto colon = e.find("://");
    r.endpoint.scheme = colon == std::string::npos ? "https" : e.substr(0, colon);
    r.endpoint.host = colon == std::string::npos ? e : e.substr(colon + 3);
    while (!r.endpoint.host.empty() && r.endpoint.host.back() == '/') r.endpoint.host.pop_back();
  }
  std::string bucket = path_style ? "/" + uri_encode(location_.bucket, true) : std::string();
  // An object's key, after the location's prefix; or the bucket itself, for a listing.
  r.path = key ? bucket + "/" + uri_encode(location_.prefix + *key, false) : (bucket.empty() ? "/" : bucket);
  return r;
}

Reply S3::send(Request req, std::string* error) {
  auto creds = credentials_ ? credentials_() : std::nullopt;
  if (!creds) {
    *error = "no AWS credentials: set keys/system/aws_access_key_id and aws_secret_access_key";
    return Reply{};
  }
  Reply r = perform(req, *creds, scratch_);
  if (!r.ok()) *error = r.error.empty() ? "status " + std::to_string(r.status) : r.error;
  return r;
}

bool S3::put_file(const std::string& key, const fs::path& file, std::string* error) {
  std::error_code ec;
  auto size = fs::file_size(file, ec);
  if (ec) return *error = "can't read " + file.string(), false;
  if (size > (5ull << 30)) return *error = "an object is at most 5 GiB in one PUT", false;
  Request r = base_request("PUT", key);
  r.upload = file;
  // A minute, plus a second for every 256 KiB: slow links still finish; stalls are cut off anyway.
  r.timeout_ms = 60000 + int64_t(size / (256 << 10)) * 1000;
  return send(std::move(r), error).ok();
}

bool S3::put(const std::string& key, std::string_view bytes, std::string* error) {
  Request r = base_request("PUT", key);
  r.headers.emplace_back("content-type", "application/octet-stream");
  r.body = std::string(bytes);
  r.timeout_ms = 60000 + int64_t(bytes.size() / (256 << 10)) * 1000;
  return send(std::move(r), error).ok();
}

bool S3::get_file(const std::string& key, const fs::path& file, std::string* error) {
  Request r = base_request("GET", key);
  r.download = file;
  r.timeout_ms = 6 * 3600 * 1000;  // stalls are cut off after two minutes; size isn't known ahead
  return send(std::move(r), error).ok();
}

std::optional<std::string> S3::get(const std::string& key, std::string* error) {
  Request r = base_request("GET", key);
  r.timeout_ms = 120000;
  Reply reply = send(std::move(r), error);
  if (!reply.ok()) return std::nullopt;
  return reply.body;
}

std::optional<std::vector<S3Object>> S3::list(const std::string& prefix, std::string* error) {
  std::vector<S3Object> out;
  std::string token;
  for (int page = 0; page < 10000; ++page) {
    Request r = base_request("GET", std::nullopt);
    r.query = {{"list-type", "2"}, {"prefix", location_.prefix + prefix}, {"encoding-type", "url"}};
    if (!token.empty()) r.query.emplace_back("continuation-token", token);
    r.timeout_ms = 60000;
    Reply reply = send(std::move(r), error);
    if (!reply.ok()) return std::nullopt;
    size_t at = 0;
    for (;;) {
      std::string item = between(reply.body, "<Contents>", "</Contents>", &at);
      if (item.empty()) break;
      S3Object o;
      o.key = url_decoded(between(item, "<Key>", "</Key>"));
      if (!o.key.starts_with(location_.prefix)) continue;
      o.key = o.key.substr(location_.prefix.size());
      o.size = std::atoll(between(item, "<Size>", "</Size>").c_str());
      o.last_modified = between(item, "<LastModified>", "</LastModified>");
      out.push_back(std::move(o));
    }
    if (between(reply.body, "<IsTruncated>", "</IsTruncated>") != "true") return out;
    token = between(reply.body, "<NextContinuationToken>", "</NextContinuationToken>");
    if (token.empty()) return *error = "a truncated listing without a continuation token", std::nullopt;
  }
  return *error = "too many pages in the listing", std::nullopt;
}

bool S3::remove(const std::string& key, std::string* error) {
  Request r = base_request("DELETE", key);
  return send(std::move(r), error).ok();
}

}  // namespace goober::aws

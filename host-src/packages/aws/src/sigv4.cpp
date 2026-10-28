// Signature Version 4 (https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv.html):
// a canonical request, hashed into a string to sign, signed with a key derived from the
// secret, the date, the region, and the service.

#include <algorithm>
#include <ctime>

#include "goober/aws/aws.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"

namespace goober::aws {
namespace {

std::string_view as_text(const std::array<uint8_t, 32>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string trim_spaces(std::string_view v) {
  // Leading and trailing spaces go, and runs of spaces inside become one (SigV4 § canonical headers).
  std::string out;
  bool space = false;
  for (char c : v) {
    if (c == ' ' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

// Every signed header, lowercase, sorted: host and x-amz-date always.
Pairs signed_headers(const SignInput& in, std::string_view amz_date, const Credentials& creds) {
  Pairs h;
  h.emplace_back("host", in.host);
  h.emplace_back("x-amz-date", std::string(amz_date));
  if (in.content_sha256_header) h.emplace_back("x-amz-content-sha256", in.payload_sha256);
  if (!creds.session_token.empty()) h.emplace_back("x-amz-security-token", creds.session_token);
  for (const auto& [k, v] : in.headers) h.emplace_back(base::to_lower(k), trim_spaces(v));
  std::sort(h.begin(), h.end());
  return h;
}

std::string signed_header_names(const Pairs& headers) {
  std::string out;
  for (const auto& [k, v] : headers) out += (out.empty() ? "" : ";") + k;
  return out;
}

}  // namespace

std::string uri_encode(std::string_view text, bool encode_slash) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || (c == '/' && !encode_slash)) {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string amz_date(int64_t unix_ms) {
  std::time_t secs = unix_ms / 1000;
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string canonical_request(const SignInput& in, std::string_view amz_date, const Credentials& creds) {
  Pairs query;
  for (const auto& [k, v] : in.query) query.emplace_back(uri_encode(k, true), uri_encode(v, true));
  std::sort(query.begin(), query.end());
  std::string query_text;
  for (const auto& [k, v] : query) query_text += (query_text.empty() ? "" : "&") + k + "=" + v;
  auto headers = signed_headers(in, amz_date, creds);
  std::string header_text;
  for (const auto& [k, v] : headers) header_text += k + ":" + v + "\n";
  return in.method + "\n" + (in.path.empty() ? "/" : in.path) + "\n" + query_text + "\n" + header_text + "\n" +
         signed_header_names(headers) + "\n" + in.payload_sha256;
}

std::string string_to_sign(std::string_view canonical, std::string_view amz_date, std::string_view region,
                           std::string_view service) {
  std::string scope = std::string(amz_date.substr(0, 8)) + "/" + std::string(region) + "/" + std::string(service) +
                      "/aws4_request";
  return "AWS4-HMAC-SHA256\n" + std::string(amz_date) + "\n" + scope + "\n" + base::sha256_hex(canonical);
}

std::array<uint8_t, 32> signing_key(std::string_view secret, std::string_view date, std::string_view region,
                                    std::string_view service) {
  auto k_date = base::hmac_sha256("AWS4" + std::string(secret), date);
  auto k_region = base::hmac_sha256(as_text(k_date), region);
  auto k_service = base::hmac_sha256(as_text(k_region), service);
  return base::hmac_sha256(as_text(k_service), "aws4_request");
}

Pairs sign(const SignInput& in, const Credentials& creds, std::string_view region, std::string_view service,
           std::string_view amz_date) {
  std::string canonical = canonical_request(in, amz_date, creds);
  std::string to_sign = string_to_sign(canonical, amz_date, region, service);
  auto key = signing_key(creds.secret_access_key, amz_date.substr(0, 8), region, service);
  std::string signature = base::hmac_sha256_hex(as_text(key), to_sign);
  auto headers = signed_headers(in, amz_date, creds);
  Pairs out;
  out.emplace_back("authorization", "AWS4-HMAC-SHA256 Credential=" + creds.access_key_id + "/" +
                                        std::string(amz_date.substr(0, 8)) + "/" + std::string(region) + "/" +
                                        std::string(service) + "/aws4_request, SignedHeaders=" +
                                        signed_header_names(headers) + ", Signature=" + signature);
  out.emplace_back("x-amz-date", std::string(amz_date));
  if (in.content_sha256_header) out.emplace_back("x-amz-content-sha256", in.payload_sha256);
  if (!creds.session_token.empty()) out.emplace_back("x-amz-security-token", creds.session_token);
  return out;
}

}  // namespace goober::aws

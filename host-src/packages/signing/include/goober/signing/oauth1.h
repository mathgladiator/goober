#pragma once

// OAuth 1.0a request signing (RFC 5849 § 3), HMAC-SHA1: the Authorization header for one request.

#include <string>
#include <utility>
#include <vector>

namespace goober::signing {

struct OAuth1 {
  std::string consumer_key, consumer_secret, token, token_secret;
  std::string nonce;      // random per request; given in tests
  std::string timestamp;  // unix seconds; given in tests
};

// RFC 3986 percent-encoding: everything but A-Z a-z 0-9 - . _ ~, hex in uppercase.
std::string rfc3986(std::string_view s);

// The signature base string and the header, for a method, a URL (its query is part of the
// signature), and the form parameters of an application/x-www-form-urlencoded body (else none).
std::string oauth1_base_string(const std::string& method, const std::string& url,
                               const std::vector<std::pair<std::string, std::string>>& form, const OAuth1& o);
std::string oauth1_signature(const std::string& base_string, const OAuth1& o);
std::string oauth1_header(const std::string& method, const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& form, const OAuth1& o);

}  // namespace goober::signing

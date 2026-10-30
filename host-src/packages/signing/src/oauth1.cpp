#include "goober/signing/oauth1.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>

#include "goober/base/encoding.h"

namespace goober::signing {

std::string rfc3986(std::string_view s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

namespace {

std::string decode(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      out += char(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> oauth_params(const OAuth1& o) {
  std::vector<std::pair<std::string, std::string>> p = {{"oauth_consumer_key", o.consumer_key},
                                                        {"oauth_nonce", o.nonce},
                                                        {"oauth_signature_method", "HMAC-SHA1"},
                                                        {"oauth_timestamp", o.timestamp},
                                                        {"oauth_version", "1.0"}};
  if (!o.token.empty()) p.emplace_back("oauth_token", o.token);
  return p;
}

}  // namespace

std::string oauth1_base_string(const std::string& method, const std::string& url,
                               const std::vector<std::pair<std::string, std::string>>& form, const OAuth1& o) {
  std::string base_url = url.substr(0, url.find_first_of("?#"));
  // The scheme and host are lowercase; a default port is dropped.
  auto scheme_end = base_url.find("://");
  if (scheme_end != std::string::npos) {
    auto host_end = base_url.find('/', scheme_end + 3);
    std::string host = base::to_lower(base_url.substr(0, host_end));
    if (host.ends_with(":443") && host.starts_with("https")) host.resize(host.size() - 4);
    if (host.ends_with(":80") && host.starts_with("http:")) host.resize(host.size() - 3);
    base_url = host + (host_end == std::string::npos ? "/" : base_url.substr(host_end));
  }
  std::vector<std::pair<std::string, std::string>> params;
  if (auto q = url.find('?'); q != std::string::npos) {
    std::string query = url.substr(q + 1, url.find('#') == std::string::npos ? std::string::npos : url.find('#') - q - 1);
    size_t i = 0;
    while (i <= query.size()) {
      size_t amp = query.find('&', i);
      if (amp == std::string::npos) amp = query.size();
      std::string pair = query.substr(i, amp - i);
      if (!pair.empty()) {
        auto eq = pair.find('=');
        params.emplace_back(decode(pair.substr(0, eq)), eq == std::string::npos ? "" : decode(pair.substr(eq + 1)));
      }
      i = amp + 1;
    }
  }
  for (const auto& kv : form) params.push_back(kv);
  for (const auto& kv : oauth_params(o)) params.push_back(kv);
  std::vector<std::pair<std::string, std::string>> encoded;
  for (const auto& [k, v] : params) encoded.emplace_back(rfc3986(k), rfc3986(v));
  std::sort(encoded.begin(), encoded.end());
  std::string joined;
  for (const auto& [k, v] : encoded) joined += (joined.empty() ? "" : "&") + k + "=" + v;
  std::string upper = method;
  for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
  return upper + "&" + rfc3986(base_url) + "&" + rfc3986(joined);
}

std::string oauth1_signature(const std::string& base_string, const OAuth1& o) {
  std::string key = rfc3986(o.consumer_secret) + "&" + rfc3986(o.token_secret);
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha1(), key.data(), int(key.size()), reinterpret_cast<const unsigned char*>(base_string.data()), base_string.size(),
       mac, &len);
  return base::base64_encode(std::string_view(reinterpret_cast<const char*>(mac), len));
}

std::string oauth1_header(const std::string& method, const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& form, const OAuth1& o) {
  std::string signature = oauth1_signature(oauth1_base_string(method, url, form, o), o);
  auto params = oauth_params(o);
  params.emplace_back("oauth_signature", signature);
  std::sort(params.begin(), params.end());
  std::string out = "OAuth ";
  for (size_t i = 0; i < params.size(); ++i) {
    out += (i ? ", " : "") + rfc3986(params[i].first) + "=\"" + rfc3986(params[i].second) + "\"";
  }
  return out;
}

}  // namespace goober::signing

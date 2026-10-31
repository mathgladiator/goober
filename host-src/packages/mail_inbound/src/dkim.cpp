#include "goober/mail_inbound/dkim.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <memory>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"

namespace goober::mail::inbound {
namespace {

constexpr size_t kMaxSignatures = 5;

bool wsp(char c) { return c == ' ' || c == '\t'; }

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (wsp(s[a]) || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (wsp(s[b - 1]) || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

// Without any whitespace (FWS inside base64 and lists).
std::string squeeze(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (!wsp(c) && c != '\r' && c != '\n') out += c;
  }
  return out;
}

// A tag=value list (RFC 6376 § 3.2): names lowercase-insensitive as written, values trimmed.
std::vector<std::pair<std::string, std::string>> tags(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = 0;
  while (at <= text.size()) {
    size_t semi = text.find(';', at);
    std::string_view part = text.substr(at, semi == std::string_view::npos ? std::string_view::npos : semi - at);
    if (auto eq = part.find('='); eq != std::string_view::npos) out.emplace_back(trim(part.substr(0, eq)), trim(part.substr(eq + 1)));
    if (semi == std::string_view::npos) break;
    at = semi + 1;
  }
  return out;
}

std::optional<std::string> tag(const std::vector<std::pair<std::string, std::string>>& t, std::string_view name) {
  for (const auto& [k, v] : t) {
    if (k == name) return v;
  }
  return std::nullopt;
}

// The DKIM-Signature value with b='s value removed (RFC 6376 § 3.7), everything else as it is.
std::string without_b(std::string_view value) {
  size_t at = 0;
  while (at < value.size()) {
    size_t start = at;
    while (start < value.size() && (wsp(value[start]) || value[start] == '\r' || value[start] == '\n')) ++start;
    size_t semi = value.find(';', start);
    size_t end = semi == std::string_view::npos ? value.size() : semi;
    std::string_view part = value.substr(start, end - start);
    size_t eq = part.find('=');
    if (eq != std::string_view::npos && trim(part.substr(0, eq)) == "b") {
      return std::string(value.substr(0, start + eq + 1)) + std::string(value.substr(end));
    }
    at = end + 1;
  }
  return std::string(value);
}

template <typename T, void (*F)(T*)>
struct Free {
  void operator()(T* p) const { F(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, Free<EVP_PKEY, EVP_PKEY_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Free<EVP_MD_CTX, EVP_MD_CTX_free>>;

// The public key from a key record's p= (SubjectPublicKeyInfo, or a bare PKCS#1 RSAPublicKey).
Pkey public_key(const std::string& algorithm, const std::string& der) {
  const auto* p = reinterpret_cast<const unsigned char*>(der.data());
  if (algorithm == "ed25519-sha256") {
    if (der.size() == 32) return Pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, p, der.size()));
    return Pkey(d2i_PUBKEY(nullptr, &p, long(der.size())));
  }
  if (EVP_PKEY* k = d2i_PUBKEY(nullptr, &p, long(der.size()))) return Pkey(k);
  p = reinterpret_cast<const unsigned char*>(der.data());
  return Pkey(d2i_PublicKey(EVP_PKEY_RSA, nullptr, &p, long(der.size())));
}

DkimResult verify_one(const Headers& headers, size_t sig_index, std::string_view body, const KeyLookup& lookup,
                      int64_t now_s) {
  DkimResult r;
  std::string err;
  const auto& raw = headers[sig_index].second;
  auto sig = parse_dkim_signature(raw, &err);
  if (!sig) return r.why = err, r;
  r.domain = sig->domain;
  if (sig->expires && *sig->expires < now_s) return r.why = "the signature expired (x=)", r;

  // The body hash.
  auto canon_body = canonical_body(body, sig->body_canon == "relaxed");
  auto bh = base::sha256(canon_body);
  if (std::string(reinterpret_cast<const char*>(bh.data()), bh.size()) != sig->body_hash) {
    return r.why = "the body hash doesn't match (the body changed after signing)", r;
  }

  // The headers it signs: for each name in h=, the next instance from the bottom.
  bool relaxed = sig->header_canon == "relaxed";
  std::string data;
  std::vector<bool> used(headers.size(), false);
  for (const auto& name : sig->signed_headers) {
    for (size_t i = headers.size(); i-- > 0;) {
      if (used[i] || i == sig_index || lower(headers[i].first) != name) continue;
      used[i] = true;
      data += canonical_header(headers[i].first, headers[i].second, relaxed);
      break;
    }
  }
  // The signature's own header, b= emptied, without its final CRLF.
  std::string self = canonical_header(headers[sig_index].first, without_b(raw), relaxed);
  if (self.size() >= 2 && self.ends_with("\r\n")) self.resize(self.size() - 2);
  data += self;

  // The key.
  std::string name = sig->selector + "._domainkey." + sig->domain;
  auto records = lookup(name, &err);
  if (!records || records->empty()) return r.why = "no DKIM key at " + name + (err.empty() ? "" : ": " + err), r;
  std::optional<std::string> key_der;
  std::string key_why = "no usable key record at " + name;
  for (const auto& rec : *records) {
    auto t = tags(rec);
    if (auto v = tag(t, "v"); v && *v != "DKIM1") continue;
    std::string k = tag(t, "k").value_or("rsa");
    if ((sig->algorithm == "rsa-sha256" && k != "rsa") || (sig->algorithm == "ed25519-sha256" && k != "ed25519")) continue;
    if (auto h = tag(t, "h"); h && squeeze(*h).find("sha256") == std::string::npos) continue;
    if (auto flags = tag(t, "t"); flags && squeeze(*flags).find('y') != std::string::npos) {
      key_why = "the key at " + name + " is in testing mode (t=y)";  // RFC 6376 § 3.6.1: treat as unsigned
      continue;
    }
    auto p = tag(t, "p");
    if (!p || squeeze(*p).empty()) {
      key_why = "the key at " + name + " is revoked (empty p=)";
      continue;
    }
    key_der = base::base64_decode(squeeze(*p));
    if (key_der) break;
  }
  if (!key_der) return r.why = key_why, r;
  Pkey key = public_key(sig->algorithm, *key_der);
  if (!key) return r.why = "the key at " + name + " doesn't parse", r;
  if (sig->algorithm == "rsa-sha256" && EVP_PKEY_get_bits(key.get()) < 1024) {
    return r.why = "the key at " + name + " is shorter than 1024 bits", r;  // RFC 8301 § 3.2
  }

  MdCtx ctx(EVP_MD_CTX_new());
  const auto* s = reinterpret_cast<const unsigned char*>(sig->signature.data());
  bool ok = false;
  if (sig->algorithm == "rsa-sha256") {
    ok = EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
         EVP_DigestVerify(ctx.get(), s, sig->signature.size(), reinterpret_cast<const unsigned char*>(data.data()),
                          data.size()) == 1;
  } else {
    // RFC 8463 § 3: Ed25519 over the SHA-256 hash of the header data.
    auto digest = base::sha256(data);
    ok = EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
         EVP_DigestVerify(ctx.get(), s, sig->signature.size(), digest.data(), digest.size()) == 1;
  }
  if (!ok) return r.why = "the signature doesn't verify against the key at " + name, r;
  r.pass = true;
  return r;
}

}  // namespace

std::optional<DkimSignature> parse_dkim_signature(std::string_view value, std::string* error) {
  auto t = tags(value);
  auto fail = [&](std::string why) { return *error = std::move(why), std::optional<DkimSignature>(); };
  // A tag twice is malformed (RFC 6376 § 3.2).
  for (size_t i = 0; i < t.size(); ++i) {
    for (size_t j = i + 1; j < t.size(); ++j) {
      if (t[i].first == t[j].first) return fail("the tag " + t[i].first + "= appears twice");
    }
  }
  DkimSignature s;
  if (tag(t, "v").value_or("") != "1") return fail("v= isn't 1");
  s.algorithm = lower(squeeze(tag(t, "a").value_or("")));
  if (s.algorithm != "rsa-sha256" && s.algorithm != "ed25519-sha256") {
    return fail("the algorithm " + (s.algorithm.empty() ? std::string("(none)") : s.algorithm) +
                " isn't accepted (rsa-sha256 or ed25519-sha256; RFC 8301 retires rsa-sha1)");
  }
  s.domain = lower(squeeze(tag(t, "d").value_or("")));
  s.selector = lower(squeeze(tag(t, "s").value_or("")));
  if (s.domain.empty() || s.selector.empty()) return fail("d= and s= are required");
  for (char c : s.domain + s.selector) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') return fail("d= or s= isn't a DNS name");
  }
  std::string c = lower(squeeze(tag(t, "c").value_or("simple/simple")));
  auto slash = c.find('/');
  s.header_canon = c.substr(0, slash);
  s.body_canon = slash == std::string::npos ? "simple" : c.substr(slash + 1);
  for (const auto& x : {s.header_canon, s.body_canon}) {
    if (x != "simple" && x != "relaxed") return fail("c= isn't simple or relaxed");
  }
  std::string h = lower(squeeze(tag(t, "h").value_or("")));
  size_t at = 0;
  while (at <= h.size() && !h.empty()) {
    size_t colon = h.find(':', at);
    std::string name = h.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
    if (!name.empty()) s.signed_headers.push_back(name);
    if (colon == std::string::npos) break;
    at = colon + 1;
  }
  if (std::find(s.signed_headers.begin(), s.signed_headers.end(), "from") == s.signed_headers.end()) {
    return fail("h= doesn't sign From");
  }
  auto bh = base::base64_decode(squeeze(tag(t, "bh").value_or("")));
  auto b = base::base64_decode(squeeze(tag(t, "b").value_or("")));
  if (!bh || bh->size() != 32 || !b || b->empty()) return fail("bh= or b= isn't base64 of the right size");
  s.body_hash = *bh;
  s.signature = *b;
  if (tag(t, "l")) return fail("l= is refused: content after the signed length would be unsigned");
  if (auto x = tag(t, "x")) {
    std::string digits = squeeze(*x);
    if (digits.empty() || digits.size() > 12 || !std::all_of(digits.begin(), digits.end(), ::isdigit)) return fail("x= isn't a time");
    s.expires = std::stoll(digits);
  }
  return s;
}

std::string canonical_header(std::string_view name, std::string_view value, bool relaxed) {
  if (!relaxed) {
    std::string out = std::string(name) + ":" + std::string(value);
    if (!out.ends_with("\r\n")) out += "\r\n";
    return out;
  }
  // Unfold, every run of whitespace to one space, none at either end of the value.
  std::string v;
  for (char c : value) {
    if (c == '\r' || c == '\n') continue;
    if (wsp(c)) {
      if (!v.empty() && v.back() != ' ') v += ' ';
      continue;
    }
    v += c;
  }
  while (!v.empty() && v.back() == ' ') v.pop_back();
  if (!v.empty() && v.front() == ' ') v.erase(0, 1);
  return lower(trim(name)) + ":" + v + "\r\n";
}

std::string canonical_body(std::string_view body, bool relaxed) {
  // Lines, without their CRLF (a bare LF counts as a line end too).
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      lines.push_back(std::move(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  bool unterminated = !cur.empty();
  if (unterminated) lines.push_back(std::move(cur));
  if (relaxed) {
    for (auto& l : lines) {
      std::string o;
      for (char c : l) {
        if (wsp(c)) {
          if (o.empty() || o.back() != ' ') o += ' ';
        } else {
          o += c;
        }
      }
      while (!o.empty() && o.back() == ' ') o.pop_back();
      l = std::move(o);
    }
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  std::string out;
  for (const auto& l : lines) out += l + "\r\n";
  if (out.empty() && !relaxed) out = "\r\n";  // simple: an empty body is one CRLF
  return out;
}

std::vector<DkimResult> verify_dkim(const Headers& headers, std::string_view body, const KeyLookup& lookup,
                                    int64_t now_s) {
  std::vector<DkimResult> out;
  for (size_t i = 0; i < headers.size() && out.size() < kMaxSignatures; ++i) {
    if (lower(trim(headers[i].first)) != "dkim-signature") continue;
    out.push_back(verify_one(headers, i, body, lookup, now_s));
  }
  return out;
}

bool dkim_aligned(std::string_view signing_domain, std::string_view from_domain) {
  std::string d = lower(signing_domain), f = lower(from_domain);
  if (d.empty() || d.find('.') == std::string::npos) return false;
  return f == d || (f.size() > d.size() && f.ends_with("." + d));
}

std::string verified_author(const Headers& headers, std::string_view body, const KeyLookup& lookup, int64_t now_s,
                            std::string* why) {
  // Exactly one From header holding exactly one address: a second From, unsigned, is how a
  // signed message is made to show another author.
  std::vector<std::string> froms;
  for (const auto& [name, value] : headers) {
    if (lower(trim(name)) == "from") froms.push_back(value);
  }
  if (froms.size() != 1) return *why = froms.empty() ? "no From header" : "more than one From header", "";
  auto boxes = parse_mailboxes(froms[0]);
  if (boxes.size() != 1 || boxes[0].address.find('@') == std::string::npos) return *why = "From doesn't hold exactly one address", "";
  std::string address = lower(boxes[0].address);
  std::string domain = address.substr(address.rfind('@') + 1);
  auto results = verify_dkim(headers, body, lookup, now_s);
  if (results.empty()) return *why = "the message has no DKIM signature", "";
  std::string first_why;
  for (const auto& r : results) {
    if (r.pass && dkim_aligned(r.domain, domain)) return address;
    if (first_why.empty()) {
      first_why = r.pass ? "the DKIM signature that passes is " + r.domain + "'s, not aligned with " + domain : r.why;
    }
  }
  return *why = first_why, "";
}

}  // namespace goober::mail::inbound

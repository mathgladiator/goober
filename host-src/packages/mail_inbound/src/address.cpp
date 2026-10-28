#include "goober/mail_inbound/address.h"

#include "goober/base/encoding.h"
#include "goober/base/names.h"

namespace goober::mail::inbound {

namespace {

// Characters a local part may hold here: RFC 5322's atext and dots, lowercase. Braces are
// left out: they mark parameters.
bool local_char(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  return std::string_view("!#$%&'*+-/=?^_`|~.").find(c) != std::string_view::npos;
}

bool param_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }

std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
  return a == std::string_view::npos ? "" : std::string(s.substr(a, b - a + 1));
}

}  // namespace

bool valid_domain(std::string_view d) {
  if (d.empty() || d.size() > 253 || d.find('.') == std::string_view::npos) return false;
  size_t label = 0;
  for (size_t i = 0; i < d.size(); ++i) {
    char c = d[i];
    if (c == '.') {
      if (label == 0 || d[i - 1] == '-') return false;
      label = 0;
      continue;
    }
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok || (label == 0 && c == '-') || ++label > 63) return false;
  }
  return label > 0 && d.back() != '-';
}

std::optional<AddressPattern> parse_address_pattern(std::string_view text, std::string* error) {
  AddressPattern p;
  p.source = std::string(text);
  auto at = text.rfind('@');
  if (at == std::string_view::npos) return *error = "a mail route's address is local@domain, local@, or *@", std::nullopt;
  std::string_view local = text.substr(0, at), domain = text.substr(at + 1);
  if (!domain.empty()) {
    if (domain != base::to_lower(domain) || !valid_domain(domain)) {
      return *error = "\"" + std::string(domain) + "\" isn't a lowercase domain name", std::nullopt;
    }
    p.domain = std::string(domain);
  }
  if (local == "*") {
    p.catch_all = true;
    return p;
  }
  if (local.empty() || local.size() > 64) return *error = "the local part is 1 to 64 characters", std::nullopt;
  if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string_view::npos) {
    return *error = "the local part can't start or end with a dot, or hold two in a row", std::nullopt;
  }
  int tokens = 0;
  std::string literal;
  for (size_t i = 0; i < local.size(); ++i) {
    char c = local[i];
    if (c == '{') {
      auto close = local.find('}', i);
      if (close == std::string_view::npos) return *error = "a { without its }", std::nullopt;
      if (!literal.empty()) p.local.push_back({false, std::move(literal), ""});
      literal.clear();
      if (!p.local.empty() && p.local.back().param) return *error = "two parameters need a literal between them", std::nullopt;
      std::string inside(local.substr(i + 1, close - i - 1));
      std::string name = trim(inside), type;
      if (auto colon = inside.find(':'); colon != std::string::npos) {
        name = trim(std::string_view(inside).substr(0, colon));
        type = trim(std::string_view(inside).substr(colon + 1));
      }
      if (!base::is_valid_name(name)) return *error = "parameter \"" + name + "\" must match [a-z][a-z0-9_]*", std::nullopt;
      for (const auto& part : p.local) {
        if (part.param && part.text == name) return *error = "parameter " + name + " appears twice", std::nullopt;
      }
      if (!type.empty() && type != "token") return *error = "a parameter's type is token, or none", std::nullopt;
      if (type == "token" && ++tokens > 1) return *error = "an address holds at most one token", std::nullopt;
      p.local.push_back({true, name, type});
      i = close;
      continue;
    }
    if (c == '}') return *error = "a } without its {", std::nullopt;
    if (!local_char(c)) {
      return *error = "\"" + std::string(1, c) + "\" can't be in a local part here (lowercase letters, digits, and !#$%&'*+-/=?^_`|~.)",
             std::nullopt;
    }
    literal += c;
  }
  if (!literal.empty()) p.local.push_back({false, std::move(literal), ""});
  return p;
}

std::string AddressPattern::shape() const {
  std::string out;
  if (catch_all) {
    out = "*";
  } else {
    for (const auto& part : local) out += part.param ? "{}" : part.text;
  }
  return out + "@" + domain;
}

std::string AddressPattern::token_param() const {
  for (const auto& part : local) {
    if (part.param && part.type == "token") return part.text;
  }
  return "";
}

std::optional<std::pair<std::string, std::string>> split_address(std::string_view address) {
  std::string a = base::to_lower(address);
  auto at = a.rfind('@');
  if (at == std::string::npos || at == 0 || at + 1 >= a.size() || a.size() > 254) return std::nullopt;
  std::string local = a.substr(0, at), domain = a.substr(at + 1);
  if (local.size() > 64 || !valid_domain(domain)) return std::nullopt;
  for (char c : local) {
    if (!local_char(c) && c != '{' && c != '}') return std::nullopt;
  }
  return std::pair{local, domain};
}

std::optional<std::map<std::string, std::string>> match_local(const AddressPattern& p, std::string_view local) {
  std::map<std::string, std::string> out;
  if (p.catch_all) return out;
  size_t pos = 0;
  for (size_t i = 0; i < p.local.size(); ++i) {
    const auto& part = p.local[i];
    if (!part.param) {
      if (local.substr(pos, part.text.size()) != part.text) return std::nullopt;
      pos += part.text.size();
      continue;
    }
    // A parameter runs to the next literal (its first occurrence), or to the end.
    size_t end = local.size();
    if (i + 1 < p.local.size()) {
      end = local.find(p.local[i + 1].text, pos + 1);
      if (end == std::string_view::npos) return std::nullopt;
    }
    std::string_view value = local.substr(pos, end - pos);
    if (value.empty() || value.size() > 64) return std::nullopt;
    for (char c : value) {
      if (!param_char(c)) return std::nullopt;
    }
    out[part.text] = std::string(value);
    pos = end;
  }
  if (pos != local.size()) return std::nullopt;
  return out;
}

}  // namespace goober::mail::inbound

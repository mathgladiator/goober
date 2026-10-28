#pragma once

// Mail route addresses (038.Mail.md; design/machine/024.MailIn.md): the address plays the
// role of an HTTP path. A pattern is
//
//   support@example.com          one address on one domain
//   support@                     that local part on every mail-enabled domain the box serves
//   *@ or *@example.com          a catch-all: any local part no other route claims
//   reply+{ticket : token}@...   parameters in the local part; "token" is a signed reply token
//   orders+{id}@                 a plain parameter: [a-z0-9._-]+
//
// Addresses are compared in lowercase, as Stalwart hands them over.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::mail::inbound {

struct AddressPattern {
  struct Part {
    bool param = false;
    std::string text;  // a literal, or the parameter's name
    std::string type;  // for a parameter: "" (plain) or "token"
  };
  std::string source;
  bool catch_all = false;   // the local part is "*"
  std::vector<Part> local;  // empty for a catch-all
  std::string domain;       // "" for every domain the box serves

  // The pattern with parameter names dropped, so two patterns that claim the same
  // addresses compare equal: "reply+{}@example.com".
  std::string shape() const;
  // The token parameter's name, or "".
  std::string token_param() const;
};

std::optional<AddressPattern> parse_address_pattern(std::string_view text, std::string* error);

// Splits "Local@Domain" into lowercase local part and domain. nullopt for anything that
// isn't one plain address.
std::optional<std::pair<std::string, std::string>> split_address(std::string_view address);

// Matches a lowercase local part (the domain is checked by the caller, who knows which
// domains a box serves). Returns the parameters, or nullopt.
std::optional<std::map<std::string, std::string>> match_local(const AddressPattern& p, std::string_view local);

bool valid_domain(std::string_view domain);

}  // namespace goober::mail::inbound

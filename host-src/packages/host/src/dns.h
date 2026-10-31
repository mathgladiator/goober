#pragma once

// DNS lookups the operator tools need beyond addresses (mail_domain_check): MX and TXT records,
// through glibc's resolver (res_nquery), with the answers parsed here so the parsing can be
// tested on canned wire-format bytes. Private to the host package.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace goober::host::dns {

struct Mx {
  int preference = 0;
  std::string host;  // lowercase, without the trailing dot
};

// The records of one type in a DNS response (RFC 1035 § 4), or nullopt with `error` when the
// message is malformed. Names may use compression pointers; a loop of pointers is malformed.
std::optional<std::vector<Mx>> parse_mx(const std::vector<uint8_t>& message, std::string* error);
// Each TXT record's character-strings joined, as SPF and DMARC read them (RFC 7208 § 3.3).
std::optional<std::vector<std::string>> parse_txt(const std::vector<uint8_t>& message, std::string* error);

// Live lookups. A name with no such records (NXDOMAIN or no data) is an empty list; nullopt
// with `error` is a resolver failure (a timeout, SERVFAIL).
std::optional<std::vector<Mx>> lookup_mx(const std::string& name, std::string* error);
std::optional<std::vector<std::string>> lookup_txt(const std::string& name, std::string* error);

// Connects to address:port within timeout_ms, reads the server's first line (an SMTP banner, like
// "220 mx.example.com ESMTP"), and says QUIT. nullopt with `error` when nothing answered.
std::optional<std::string> smtp_banner(const std::string& address, uint16_t port, int timeout_ms, std::string* error);

}  // namespace goober::host::dns

#pragma once

// DKIM verification (RFC 6376, with RFC 8301's and RFC 8463's algorithms) of a message as
// Stalwart's MTA hook hands it over: headers as raw [name, value] pairs (the value as sent,
// leading space, folding, and the line's CRLF) and the body alone, CRLF. The host verifies, so
// a box's mail route can be told who wrote, as a verified claim (design/machine/024.MailIn.md
// § 5; 038 § 10). DNS is the caller's: `KeyLookup` answers the TXT records at a name.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/mail_inbound/mime.h"

namespace goober::mail::inbound {

struct DkimSignature {
  std::string algorithm;      // rsa-sha256 or ed25519-sha256
  std::string domain;         // d=, lowercased
  std::string selector;       // s=
  std::string header_canon;   // simple or relaxed
  std::string body_canon;     // simple or relaxed
  std::vector<std::string> signed_headers;  // h=, lowercased, in order
  std::string body_hash;      // bh=, decoded
  std::string signature;      // b=, decoded
  bool has_length = false;    // l= (refused: content past it would be unsigned)
  std::optional<int64_t> expires;  // x=, seconds
};

// The tags of a DKIM-Signature value, checked; nullopt with `error` when it's malformed or uses
// what isn't accepted (rsa-sha1, l=, a missing required tag, h= without from).
std::optional<DkimSignature> parse_dkim_signature(std::string_view value, std::string* error);

// Canonical forms (RFC 6376 § 3.4). A header's `value` is raw, as the hook gives it.
std::string canonical_header(std::string_view name, std::string_view value, bool relaxed);
std::string canonical_body(std::string_view body, bool relaxed);

// The TXT records at a DNS name (each record's strings joined), or nullopt with `error`.
using KeyLookup = std::function<std::optional<std::vector<std::string>>(const std::string& name, std::string* error)>;

struct DkimResult {
  bool pass = false;
  std::string domain;  // d= of the signature this result is for ("" when none parsed)
  std::string why;     // when it didn't pass
};

// Every DKIM-Signature on the message (at most 5), each verified on its own, in order.
// `now_s` is the time in seconds, for x=.
std::vector<DkimResult> verify_dkim(const Headers& headers, std::string_view body, const KeyLookup& lookup,
                                    int64_t now_s);

// Whether a signing domain is aligned with the From address's domain (relaxed alignment,
// RFC 7489 § 3.1.1): the same name, or From's is a subdomain of it.
bool dkim_aligned(std::string_view signing_domain, std::string_view from_domain);

// The verified author of a message: its one From address, lowercased, when some DKIM signature
// that passes is aligned with its domain (DMARC passes on DKIM alone). Else "" with `why`.
std::string verified_author(const Headers& headers, std::string_view body, const KeyLookup& lookup, int64_t now_s,
                            std::string* why);

}  // namespace goober::mail::inbound

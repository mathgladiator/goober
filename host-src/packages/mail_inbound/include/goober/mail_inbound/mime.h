#pragma once

// A received message, parsed into what a mail route's box sees (038 § Converting mail into
// routes; design/machine/024.MailIn.md): addresses, subject, the text and HTML bodies, and
// attachments, all decoded to UTF-8. MIME is a swamp; this parser is bounded (depth, parts,
// sizes) and never throws on bad input: what it can't read it leaves out.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goober::mail::inbound {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Mailbox {
  std::string address;  // lowercase
  std::string name;     // the display name, decoded
};

struct Part {
  std::string filename;      // decoded; "" when the part names none
  std::string content_type;  // lowercase, like application/pdf
  std::string content_id;    // for inline parts of a multipart/related, without <>
  bool inline_part = false;
  std::string bytes;         // decoded content
};

struct Message {
  Headers headers;  // unfolded, values decoded from RFC 2047 encoded words
  std::optional<Mailbox> from;
  std::vector<Mailbox> to, cc, reply_to;
  std::string subject, message_id, in_reply_to, date;
  std::vector<std::string> references;
  std::string text;  // the first text/plain body, or the HTML made into text
  std::string html;  // the first text/html body, or ""
  std::vector<Part> attachments;
  bool truncated = false;  // limits cut something off
};

struct Limits {
  int max_depth = 8;
  int max_parts = 100;
  size_t max_text_bytes = 1 << 20;  // per body
};

// Parses a message given as Stalwart gives it: the top-level headers as (name, value) pairs
// (values as on the wire, possibly folded), and the raw body after the blank line.
Message parse_message(const Headers& headers, std::string_view body, const Limits& limits = {});
// Parses a whole raw message (headers, blank line, body).
Message parse_raw_message(std::string_view raw, const Limits& limits = {});

// The first value of a header, by case-insensitive name, or nullopt.
std::optional<std::string> header(const Headers& headers, std::string_view name);

// RFC 2047 encoded words decoded to UTF-8 (B and Q, in UTF-8, US-ASCII, ISO-8859-1, and
// Windows-1252); anything else is kept as it is, made valid UTF-8.
std::string decode_words(std::string_view value);

// Addresses in a header like To or From: "Ann <ann@x.org>, bob@y.org".
std::vector<Mailbox> parse_mailboxes(std::string_view value);

// Mail no person typed, which loops feed on (038 § Mail loops die at the host): an
// Auto-Submitted other than "no", X-Autoreply or X-Autorespond, Precedence auto_reply, bulk,
// or junk, a delivery or read report, or an empty envelope sender (a bounce).
bool is_automatic(const Headers& headers, std::string_view envelope_from);

}  // namespace goober::mail::inbound

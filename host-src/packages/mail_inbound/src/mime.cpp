#include "goober/mail_inbound/mime.h"

#include <cstdint>
#include <map>

#include "goober/base/encoding.h"
#include "goober/render/markdown.h"

namespace goober::mail::inbound {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return false;
  }
  return true;
}

std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string_view::npos ? "" : std::string(s.substr(a, b - a + 1));
}

// Folding is a line break before whitespace: drop the break, keep the whitespace.
std::string unfold(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    if (c != '\r' && c != '\n') out += c;
  }
  return trim(out);
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Windows-1252's 0x80 to 0x9F; 0 where it defines nothing.
const uint16_t kCp1252[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

// Bytes in a charset, as UTF-8. Unknown charsets are treated as UTF-8, made valid.
std::string to_utf8(std::string_view bytes, std::string_view charset) {
  std::string cs = base::to_lower(trim(charset));
  bool latin1 = cs == "iso-8859-1" || cs == "latin1" || cs == "iso8859-1" || cs == "l1" || cs == "iso-8859-15";
  bool cp1252 = cs == "windows-1252" || cs == "cp1252";
  if (!latin1 && !cp1252) return base::valid_utf8(bytes);
  std::string out;
  out.reserve(bytes.size() + bytes.size() / 4);
  for (unsigned char c : bytes) {
    uint32_t cp = c;
    if (cp1252 && c >= 0x80 && c <= 0x9F) cp = kCp1252[c - 0x80] ? kCp1252[c - 0x80] : 0xFFFD;
    append_utf8(out, cp);
  }
  return out;
}

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

std::string decode_base64_lenient(std::string_view in) {
  std::string clean;
  clean.reserve(in.size());
  for (char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/') clean += c;
  }
  // Whatever padding the sender wrote, the length decides it.
  if (clean.size() % 4 == 1) clean.pop_back();
  while (clean.size() % 4) clean += '=';
  return base::base64_decode(clean).value_or("");
}

std::string decode_quoted_printable(std::string_view in, bool q_word) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (q_word && c == '_') {
      out += ' ';
    } else if (c == '=') {
      if (i + 1 < in.size() && (in[i + 1] == '\r' || in[i + 1] == '\n')) {  // a soft line break
        ++i;
        if (in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n') ++i;
      } else if (i + 2 < in.size() && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
        out += char(hex(in[i + 1]) * 16 + hex(in[i + 2]));
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

// A header value with parameters, like Content-Type or Content-Disposition.
struct Params {
  std::string value;  // lowercase
  std::map<std::string, std::string> params;  // lowercase names, decoded values
};

Params parse_params(std::string_view raw) {
  Params p;
  std::string s = unfold(raw);
  size_t i = s.find(';');
  p.value = base::to_lower(trim(std::string_view(s).substr(0, i)));
  // RFC 2231 continuations (name*0, name*1*) are gathered, then joined in order.
  std::map<std::string, std::map<int, std::pair<std::string, bool>>> pieces;
  while (i != std::string::npos && i < s.size()) {
    ++i;
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) break;
    std::string name = base::to_lower(trim(std::string_view(s).substr(i, eq - i)));
    size_t j = eq + 1;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    std::string value;
    if (j < s.size() && s[j] == '"') {
      for (++j; j < s.size() && s[j] != '"'; ++j) {
        if (s[j] == '\\' && j + 1 < s.size()) ++j;
        value += s[j];
      }
      i = s.find(';', j);
    } else {
      size_t end = s.find(';', j);
      value = trim(std::string_view(s).substr(j, end == std::string::npos ? std::string::npos : end - j));
      i = end;
    }
    bool extended = !name.empty() && name.back() == '*';
    if (extended) name.pop_back();
    int index = -1;
    if (auto star = name.find('*'); star != std::string::npos) {
      try {
        index = std::stoi(name.substr(star + 1));
      } catch (...) {
        index = 0;
      }
      name = name.substr(0, star);
    }
    if (index < 0) {
      if (extended) {
        pieces[name][0] = {value, true};
      } else if (!p.params.count(name)) {
        p.params[name] = value;
      }
    } else if (index < 100) {
      pieces[name][index] = {value, extended};
    }
  }
  for (auto& [name, parts] : pieces) {
    std::string charset, joined;
    bool first = true;
    for (auto& [index, piece] : parts) {
      std::string v = piece.first;
      if (piece.second) {
        if (first) {
          // charset'language'percent-encoded
          auto a = v.find('\''), b = a == std::string::npos ? a : v.find('\'', a + 1);
          if (b != std::string::npos) {
            charset = v.substr(0, a);
            v = v.substr(b + 1);
          }
        }
        std::string decoded;
        for (size_t k = 0; k < v.size(); ++k) {
          if (v[k] == '%' && k + 2 < v.size() && hex(v[k + 1]) >= 0 && hex(v[k + 2]) >= 0) {
            decoded += char(hex(v[k + 1]) * 16 + hex(v[k + 2]));
            k += 2;
          } else {
            decoded += v[k];
          }
        }
        v = decoded;
      }
      joined += v;
      first = false;
    }
    p.params[name] = to_utf8(joined, charset.empty() ? "utf-8" : charset);
  }
  return p;
}

Headers parse_header_block(std::string_view block) {
  Headers out;
  size_t pos = 0;
  while (pos < block.size()) {
    size_t nl = block.find('\n', pos);
    std::string_view line = block.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? block.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    if ((line[0] == ' ' || line[0] == '\t') && !out.empty()) {
      out.back().second += std::string(line);
      continue;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;
    out.emplace_back(trim(line.substr(0, colon)), std::string(line.substr(colon + 1)));
  }
  return out;
}

// Splits an entity at its first blank line.
std::pair<std::string_view, std::string_view> split_entity(std::string_view raw) {
  size_t crlf = raw.find("\r\n\r\n"), lf = raw.find("\n\n");
  if (raw.starts_with("\r\n")) return {"", raw.substr(2)};
  if (raw.starts_with("\n")) return {"", raw.substr(1)};
  if (crlf != std::string_view::npos && (lf == std::string_view::npos || crlf < lf)) return {raw.substr(0, crlf), raw.substr(crlf + 4)};
  if (lf != std::string_view::npos) return {raw.substr(0, lf), raw.substr(lf + 2)};
  return {raw, ""};
}

// The parts of a multipart body, between its boundary lines.
std::vector<std::string_view> split_multipart(std::string_view body, const std::string& boundary) {
  std::vector<std::string_view> parts;
  std::string delim = "--" + boundary;
  size_t pos = 0;
  std::optional<size_t> start;
  while (pos <= body.size()) {
    size_t nl = body.find('\n', pos);
    size_t end = nl == std::string_view::npos ? body.size() : nl;
    std::string_view line = body.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with(delim)) {
      std::string_view rest = line.substr(delim.size());
      bool closing = rest.starts_with("--");
      if (trim(closing ? rest.substr(2) : rest).empty()) {
        if (start) {
          // The line break before a boundary belongs to the boundary.
          size_t stop = pos;
          if (stop > *start && body[stop - 1] == '\n') --stop;
          if (stop > *start && body[stop - 1] == '\r') --stop;
          parts.push_back(body.substr(*start, stop - *start));
        }
        if (closing) return parts;
        start = nl == std::string_view::npos ? body.size() : nl + 1;
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  if (start && *start < body.size()) parts.push_back(body.substr(*start));  // no closing line
  return parts;
}

std::string cut_text(std::string s, size_t max, bool* truncated) {
  if (s.size() <= max) return s;
  *truncated = true;
  return base::valid_utf8(std::string_view(s).substr(0, max));
}

struct Walker {
  const Limits& limits;
  Message& m;
  int parts = 0;

  void entity(const Headers& headers, std::string_view body, int depth, bool in_related) {
    if (++parts > limits.max_parts || depth > limits.max_depth) {
      m.truncated = true;
      return;
    }
    auto ct = parse_params(header(headers, "content-type").value_or("text/plain"));
    if (ct.value.empty() || ct.value.find('/') == std::string::npos) ct.value = "text/plain";
    if (ct.value.starts_with("multipart/")) {
      auto boundary = ct.params.find("boundary");
      if (boundary == ct.params.end() || boundary->second.empty()) return;
      bool related = ct.value == "multipart/related";
      for (auto part : split_multipart(body, boundary->second)) {
        auto [head, inner] = split_entity(part);
        entity(parse_header_block(head), inner, depth + 1, related);
      }
      return;
    }
    std::string encoding = base::to_lower(trim(header(headers, "content-transfer-encoding").value_or("7bit")));
    std::string bytes;
    if (encoding == "base64") {
      bytes = decode_base64_lenient(body);
    } else if (encoding == "quoted-printable") {
      bytes = decode_quoted_printable(body, false);
    } else {
      bytes = std::string(body);
    }
    auto disposition = parse_params(header(headers, "content-disposition").value_or(""));
    std::string filename = disposition.params.count("filename") ? disposition.params["filename"]
                           : ct.params.count("name")            ? ct.params["name"]
                                                                : "";
    filename = decode_words(filename);
    bool attachment = disposition.value == "attachment" || (!filename.empty() && disposition.value != "inline" &&
                                                            !ct.value.starts_with("text/"));
    if (!attachment && ct.value == "text/plain" && m.text.empty() && disposition.value != "attachment") {
      m.text = cut_text(to_utf8(bytes, ct.params["charset"]), limits.max_text_bytes, &m.truncated);
      return;
    }
    if (!attachment && ct.value == "text/html" && m.html.empty()) {
      m.html = cut_text(to_utf8(bytes, ct.params["charset"]), limits.max_text_bytes, &m.truncated);
      return;
    }
    Part a;
    a.content_type = ct.value;
    a.filename = filename.empty() && ct.value == "message/rfc822" ? "message.eml" : filename;
    a.content_id = trim(header(headers, "content-id").value_or(""));
    if (a.content_id.size() >= 2 && a.content_id.front() == '<' && a.content_id.back() == '>') {
      a.content_id = a.content_id.substr(1, a.content_id.size() - 2);
    }
    a.inline_part = !attachment && (in_related || disposition.value == "inline");
    a.bytes = std::move(bytes);
    m.attachments.push_back(std::move(a));
  }
};

std::string strip_angles(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '<' && s.back() == '>') s = s.substr(1, s.size() - 2);
  return s;
}

}  // namespace

std::optional<std::string> header(const Headers& headers, std::string_view name) {
  for (const auto& [k, v] : headers) {
    if (ieq(k, name)) return v;
  }
  return std::nullopt;
}

std::string decode_words(std::string_view in) {
  // A header worth decoding is short: past 64 KiB the rest is dropped. Each encoded word is
  // looked for within 1 KiB of its "=?" (RFC 2047 says 75; senders run longer), so a header
  // of "=?" after "=?" is linear, not quadratic.
  constexpr size_t kMaxHeader = 64 * 1024, kMaxWord = 1024;
  if (in.size() > kMaxHeader) in = in.substr(0, kMaxHeader);
  std::string out;
  size_t i = 0;
  bool last_was_word = false;
  std::string pending_space;
  while (i < in.size()) {
    if (in.substr(i, 2) == "=?") {
      std::string_view win = in.substr(i, kMaxWord);
      size_t q1 = win.find('?', 2);
      size_t q2 = q1 == std::string_view::npos || q1 + 2 >= win.size() || win[q1 + 2] != '?' ? std::string_view::npos : q1 + 2;
      size_t end = q2 == std::string_view::npos ? q2 : win.find("?=", q2 + 1);
      if (end != std::string_view::npos) {
        q1 += i;
        q2 += i;
        end += i;
        std::string charset(in.substr(i + 2, q1 - i - 2));
        if (auto star = charset.find('*'); star != std::string::npos) charset.resize(star);  // RFC 2231 language
        char enc = in[q1 + 1];
        std::string_view text = in.substr(q2 + 1, end - q2 - 1);
        std::string bytes;
        bool ok = true;
        if (enc == 'B' || enc == 'b') {
          bytes = decode_base64_lenient(text);
        } else if (enc == 'Q' || enc == 'q') {
          bytes = decode_quoted_printable(text, true);
        } else {
          ok = false;
        }
        if (ok) {
          // Whitespace between two encoded words is dropped (RFC 2047 § 6.2).
          if (!last_was_word) out += pending_space;
          pending_space.clear();
          out += to_utf8(bytes, charset);
          last_was_word = true;
          i = end + 2;
          continue;
        }
      }
    }
    char c = in[i];
    if (c == ' ' || c == '\t') {
      pending_space += c;
    } else {
      out += pending_space;
      pending_space.clear();
      out += c;
      last_was_word = false;
    }
    ++i;
  }
  out += pending_space;
  return base::valid_utf8(out);
}

std::vector<Mailbox> parse_mailboxes(std::string_view value_in) {
  std::string value = unfold(value_in);
  std::vector<Mailbox> out;
  // Split at commas outside quotes, angle brackets, and comments.
  std::vector<std::string> items;
  std::string cur;
  int angle = 0, paren = 0;
  bool quoted = false;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (quoted) {
      cur += c;
      if (c == '\\' && i + 1 < value.size()) cur += value[++i];
      else if (c == '"') quoted = false;
      continue;
    }
    if (c == '"') quoted = true;
    if (c == '<') ++angle;
    if (c == '>' && angle) --angle;
    if (c == '(') ++paren;
    if (c == ')' && paren) --paren;
    if ((c == ',' || c == ';') && !angle && !paren) {
      items.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  items.push_back(cur);
  for (auto item : items) {
    // A group's name ("undisclosed-recipients:") isn't an address.
    if (auto colon = item.find(':'); colon != std::string::npos && item.find('<') == std::string::npos &&
                                     item.find('"') == std::string::npos) {
      item = item.substr(colon + 1);
    }
    std::string addr, name;
    auto lt = item.rfind('<'), gt = item.rfind('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
      addr = trim(std::string_view(item).substr(lt + 1, gt - lt - 1));
      name = trim(std::string_view(item).substr(0, lt));
    } else {
      // bare address, maybe with a (comment)
      std::string bare;
      int depth = 0;
      for (char c : item) {
        if (c == '(') ++depth;
        else if (c == ')' && depth) --depth;
        else if (!depth) bare += c;
      }
      addr = trim(bare);
    }
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
      std::string unq;
      for (size_t i = 1; i + 1 < name.size(); ++i) {
        if (name[i] == '\\' && i + 2 < name.size()) ++i;
        unq += name[i];
      }
      name = unq;
    }
    auto at = addr.rfind('@');
    if (addr.empty() || at == std::string::npos || at == 0 || at + 1 == addr.size() ||
        addr.find_first_of(" \t\"<>,;") != std::string::npos) {
      continue;
    }
    out.push_back(Mailbox{base::to_lower(addr), decode_words(name)});
  }
  return out;
}

Message parse_message(const Headers& raw_headers, std::string_view body, const Limits& limits) {
  Message m;
  for (const auto& [k, v] : raw_headers) m.headers.emplace_back(trim(k), decode_words(unfold(v)));
  Walker w{limits, m};
  // The entity walk reads the raw values (parameters decode their own way).
  Headers raw;
  for (const auto& [k, v] : raw_headers) raw.emplace_back(trim(k), unfold(v));
  w.entity(raw, body, 0, false);

  auto get = [&](const char* name) { return header(m.headers, name).value_or(""); };
  auto raw_get = [&](const char* name) { return header(raw, name).value_or(""); };
  if (auto from = parse_mailboxes(raw_get("from")); !from.empty()) m.from = from[0];
  m.to = parse_mailboxes(raw_get("to"));
  m.cc = parse_mailboxes(raw_get("cc"));
  m.reply_to = parse_mailboxes(raw_get("reply-to"));
  m.subject = get("subject");
  m.message_id = strip_angles(get("message-id"));
  m.in_reply_to = strip_angles(get("in-reply-to"));
  m.date = get("date");
  std::string refs = get("references");
  for (size_t i = 0; i < refs.size();) {
    size_t a = refs.find('<', i);
    if (a == std::string::npos) break;
    size_t b = refs.find('>', a);
    if (b == std::string::npos) break;
    m.references.push_back(refs.substr(a + 1, b - a - 1));
    i = b + 1;
  }
  if (m.text.empty() && !m.html.empty()) {
    m.text = render::html_to_text(m.html);
  }
  return m;
}

Message parse_raw_message(std::string_view raw, const Limits& limits) {
  auto [head, body] = split_entity(raw);
  return parse_message(parse_header_block(head), body, limits);
}

bool is_automatic(const Headers& headers, std::string_view envelope_from) {
  if (envelope_from.empty()) return true;  // a bounce (RFC 5321 § 4.5.5)
  for (const auto& [k, v_raw] : headers) {
    std::string v = base::to_lower(trim(v_raw));
    if (ieq(k, "auto-submitted") && !v.starts_with("no")) return true;
    if (ieq(k, "x-autoreply") || ieq(k, "x-autorespond")) return true;
    if (ieq(k, "precedence") && (v == "auto_reply" || v == "bulk" || v == "junk")) return true;
    if (ieq(k, "content-type") && v.starts_with("multipart/report")) return true;
  }
  return false;
}

}  // namespace goober::mail::inbound

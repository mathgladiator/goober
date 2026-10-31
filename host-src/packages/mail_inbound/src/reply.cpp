#include "goober/mail_inbound/reply.h"

#include <vector>

#include "goober/base/encoding.h"

namespace goober::mail::inbound {

namespace {

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return std::string(s.substr(a, b - a));
}

bool starts_ci(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && base::to_lower(s.substr(0, p.size())) == p;
}

bool ends_ci(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && base::to_lower(s.substr(s.size() - p.size())) == p;
}

// "On Tue, Oct 29, 2026 at 6:00 AM The Example Club <thesis@example.com> wrote:", in the
// languages mail clients ship most, sometimes wrapped onto a second line.
bool quote_header(std::string_view line) {
  static const std::pair<const char*, const char*> kHeaders[] = {
      {"on ", "wrote:"}, {"le ", "a écrit :"}, {"le ", "a écrit:"}, {"am ", "schrieb:"}, {"el ", "escribió:"},
      {"il ", "ha scritto:"}, {"op ", "geschreven:"}, {"em ", "escreveu:"}};
  for (const auto& [start, end] : kHeaders) {
    if (starts_ci(line, start) && ends_ci(line, end)) return true;
  }
  return false;
}

}  // namespace

std::string reply_text(std::string_view text) {
  std::vector<std::string> lines;
  size_t i = 0;
  while (i <= text.size()) {
    size_t end = text.find('\n', i);
    if (end == std::string_view::npos) end = text.size();
    lines.push_back(trim(text.substr(i, end - i)));
    if (end == text.size()) break;
    i = end + 1;
  }
  size_t keep = lines.size();
  for (size_t n = 0; n < lines.size() && keep == lines.size(); ++n) {
    const std::string& l = lines[n];
    std::string lower = base::to_lower(l);
    bool cut = l.starts_with(">") || quote_header(l) || lower.find("reply above this line") != std::string::npos ||
               lower.starts_with("-----original message-----") || lower.starts_with("-----ursprüngliche nachricht") ||
               (l.size() >= 20 && l.find_first_not_of('_') == std::string::npos) ||  // Outlook's rule
               l == "--" || text.substr(0).empty() || lower.starts_with("sent from my ") || lower.starts_with("get outlook for ");
    // "On ... <x@y>" with "wrote:" wrapped onto the next line.
    if (!cut && starts_ci(l, "on ") && n + 1 < lines.size() && ends_ci(lines[n + 1], "wrote:")) cut = true;
    // An Outlook header block: From: then Sent: or Date: within the next three lines.
    if (!cut && starts_ci(l, "from:")) {
      for (size_t k = n + 1; k < lines.size() && k <= n + 3; ++k) {
        if (starts_ci(lines[k], "sent:") || starts_ci(lines[k], "date:")) cut = true;
      }
    }
    if (cut) keep = n;  // ("--" above is a signature's "-- ", trimmed)
  }
  std::string out;
  for (size_t n = 0; n < keep; ++n) out += lines[n] + "\n";
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  size_t lead = out.find_first_not_of('\n');
  return lead == std::string::npos ? "" : out.substr(lead);
}

}  // namespace goober::mail::inbound

// The log dive (design/machine/032.Logs.md § 5): newest first, across one log or several, within
// a time window, by substring, regex, fields, or an address's hash, reading only the files whose
// span overlaps the window and at most max_read bytes.

#include "goober/base/time.h"
#include <algorithm>
#include <deque>
#include <fstream>

#include "goober/logs/logs.h"

namespace goober::logs {

namespace fs = std::filesystem;

namespace {

std::optional<int64_t> at_of_line(std::string_view line) {
  auto k = line.find("\"at\":\"");
  if (k == std::string_view::npos || k + 6 + 24 > line.size()) return std::nullopt;
  return parse_at(line.substr(k + 6, 24));
}

std::string text_of(const bj::value& v) {
  if (v.is_string()) return std::string(v.as_string());
  return json::dump(v);
}

}  // namespace

Found search(const fs::path& dir, const std::vector<std::string>& names, const Query& q) {
  Found found;
  std::vector<Hit> all;
  for (const auto& name : names) {
    auto files = files_of(dir, name);
    std::deque<Hit> mine;  // this log's newest matches, oldest first
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
      if (mine.size() >= q.limit) {
        found.truncated = true;  // older files may hold more
        break;
      }
      // A file whose span is outside the window is skipped without reading it through.
      std::ifstream probe(*it, std::ios::binary);
      std::string first;
      std::getline(probe, first);
      auto first_at = at_of_line(first);
      if (first_at && *first_at > q.until_ms) continue;
      std::ifstream in(*it, std::ios::binary);
      std::deque<Hit> file_hits;
      std::string line;
      while (std::getline(in, line)) {
        found.bytes_read += line.size() + 1;
        if (found.bytes_read > q.max_read) {
          found.truncated = true;
          break;
        }
        if (line.empty()) continue;
        if (!q.pattern.empty() && line.find(q.pattern) == std::string::npos) continue;
        if (q.deadline_us && base::monotonic_us() > q.deadline_us) {
          found.truncated = true;
          break;
        }
        if (q.regex && !std::regex_search(line.begin(), line.begin() + std::min(line.size(), kRegexLineBytes), *q.regex)) continue;
        auto at = at_of_line(line);
        if (!at || *at < q.since_ms) continue;
        if (*at > q.until_ms) break;
        std::string err;
        auto v = json::parse(line, &err);
        if (!v || !v->is_object()) continue;
        const auto& o = v->as_object();
        int64_t seq = o.contains("seq") && o.at("seq").is_int64() ? o.at("seq").as_int64() : 0;
        if (!q.before_log.empty()) {
          if (name == q.before_log ? seq >= q.before_seq : *at >= q.before_at_ms) continue;
        }
        bool match = true;
        for (const auto& [k, want] : q.fields) {
          auto* f = o.if_contains(k);
          if (!f || text_of(*f) != want) match = false;
        }
        if (match && !q.hashes.empty()) {
          bool any = false;
          for (const auto& kv : o) {
            if (!kv.key().ends_with("_h") || !kv.value().is_string()) continue;
            for (const auto& h : q.hashes) any |= kv.value().as_string() == h;
          }
          match = any;
        }
        if (!match || (q.allow && !q.allow(name, o))) continue;
        file_hits.push_back(Hit{name, seq, *at, line});
        if (file_hits.size() > q.limit) {
          file_hits.pop_front();
          found.truncated = true;  // more matched than the limit: a cursor reaches them
        }
      }
      // This file is newer than what's already in mine: its hits go after them... but mine
      // holds newer files' hits, so this older file's go before.
      for (auto h = file_hits.rbegin(); h != file_hits.rend(); ++h) mine.push_front(std::move(*h));
      while (mine.size() > q.limit) {
        mine.pop_front();
        found.truncated = true;
      }
      if (found.bytes_read > q.max_read) break;
      // Older files are older still: once this one starts before the window, stop.
      if (first_at && *first_at < q.since_ms) break;
    }
    for (auto& h : mine) all.push_back(std::move(h));
    if (found.bytes_read > q.max_read) break;
  }
  std::stable_sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) {
    return a.at_ms != b.at_ms ? a.at_ms > b.at_ms : a.seq > b.seq;
  });
  if (all.size() > q.limit) {
    all.resize(q.limit);
    found.truncated = true;
  }
  found.hits = std::move(all);
  return found;
}

bool safe_regex(std::string_view p, std::string* why) {
  if (p.size() > 200) return *why = "a regex is at most 200 characters", false;
  // Per open group: whether it holds a quantifier yet.
  std::vector<bool> groups;
  bool last_quantified_group = false;
  for (size_t i = 0; i < p.size(); ++i) {
    char c = p[i];
    bool quant = c == '*' || c == '+' || c == '?' || c == '{';
    if (c == '\\') {
      if (i + 1 < p.size() && p[i + 1] >= '1' && p[i + 1] <= '9') return *why = "backreferences aren't allowed", false;
      ++i;
      last_quantified_group = false;
      continue;
    }
    if (c == '[') {  // a class: skip to its end
      size_t j = i + 1;
      if (j < p.size() && p[j] == ']') ++j;
      while (j < p.size() && p[j] != ']') j += p[j] == '\\' ? 2 : 1;
      i = j;
      last_quantified_group = false;
      continue;
    }
    if (c == '(') {
      groups.push_back(false);
      last_quantified_group = false;
      continue;
    }
    if (c == '|') {
      // Alternatives under a repeat ((a|aa)*) backtrack as badly as a repeat inside one.
      for (auto&& g : groups) g = true;
      continue;
    }
    if (c == ')') {
      last_quantified_group = !groups.empty() && groups.back();
      if (!groups.empty()) groups.pop_back();
      continue;
    }
    if (quant) {
      if (last_quantified_group) {
        return *why = "a repeated group can't hold a repeat or alternatives, like (a+)+ or (a|aa)*: it can run for ever", false;
      }
      if (!groups.empty()) groups.back() = true;
      for (auto&& g : groups) g = true;  // every enclosing group now holds one
    }
    last_quantified_group = false;
  }
  return true;
}

}  // namespace goober::logs

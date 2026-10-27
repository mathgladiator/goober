#include "goober/render/frontmatter.h"

#include <charconv>
#include <vector>

namespace goober::render {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool is_key(std::string_view k) {
  if (k.empty()) return false;
  for (char c : k) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bj::value scalar(std::string_view v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    // A double-quoted value is a JSON string, so it can hold quotes and newlines.
    std::string err;
    if (auto s = json::parse(v, &err); s && s->is_string()) return *s;
    return bj::string(v.substr(1, v.size() - 2));
  }
  if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return bj::string(v.substr(1, v.size() - 2));
  if (v == "true") return true;
  if (v == "false") return false;
  if (!v.empty() && v.size() < 19) {
    int64_t n = 0;
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec == std::errc() && end == v.data() + v.size()) return n;
  }
  return bj::string(v);
}

// The next line of text starting at `pos`, without its newline; advances `pos`.
std::string_view next_line(std::string_view text, size_t& pos) {
  size_t end = text.find('\n', pos);
  std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
  pos = end == std::string_view::npos ? text.size() : end + 1;
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

}  // namespace

Document split_front_matter(std::string_view text, std::string* error) {
  Document doc;
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  size_t pos = 0;
  if (trim(next_line(text, pos)) != "---") {
    doc.body = std::string(text);
    return doc;
  }
  std::string list_key;
  std::vector<std::string> bare_keys;  // "key:" lines, which are lists only if items follow
  bool closed = false;
  while (pos < text.size()) {
    std::string_view raw = next_line(text, pos);
    std::string_view line = trim(raw);
    if (line == "---" || line == "...") {
      closed = true;
      break;
    }
    if (line.empty() || line.front() == '#') continue;
    if (!list_key.empty() && line.starts_with("- ") && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '-')) {
      auto& arr = doc.fields[list_key];
      if (!arr.is_array()) arr = bj::array();
      arr.as_array().push_back(scalar(trim(line.substr(2))));
      continue;
    }
    list_key.clear();
    auto colon = line.find(':');
    std::string_view key = colon == std::string_view::npos ? std::string_view() : trim(line.substr(0, colon));
    if (!is_key(key)) {
      if (error) *error = "front matter line is not \"key: value\": " + std::string(line);
      continue;
    }
    std::string_view value = trim(line.substr(colon + 1));
    if (value.empty()) {
      list_key = std::string(key);
      bare_keys.push_back(list_key);
      doc.fields[key] = bj::array();
      continue;
    }
    if (value.front() == '[' || value.front() == '{') {
      std::string err;
      if (auto v = json::parse(value, &err)) {
        doc.fields[key] = std::move(*v);
        continue;
      }
      if (error) *error = "front matter " + std::string(key) + " is not valid JSON: " + err;
      continue;
    }
    doc.fields[key] = scalar(value);
  }
  if (!closed) {
    // An unclosed block isn't front matter. Say so: a page that meant to have front
    // matter, and maybe requirements, must not quietly become a public page.
    if (error) *error = "front matter opens with --- but never closes";
    doc.fields.clear();
    doc.body = std::string(text);
    return doc;
  }
  // A "key:" with no items is an empty string, not an empty list; "key: []" stays a list.
  for (const auto& k : bare_keys) {
    auto* v = doc.fields.if_contains(k);
    if (v && v->is_array() && v->as_array().empty()) *v = bj::string();
  }
  doc.has_front_matter = true;
  doc.body = std::string(text.substr(pos));
  return doc;
}

}  // namespace goober::render

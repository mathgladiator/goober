#include "goober/skills/skills.h"

#include <algorithm>
#include <limits>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/render/frontmatter.h"

namespace goober::skills {
namespace {

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
  return std::string(s);
}

bool lower_or_digit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

// A one-line field: trimmed, at most `max` bytes, no line breaks or other control bytes.
bool one_line(std::string& v, const char* field, size_t max, std::string* error) {
  v = trim(v);
  if (v.empty()) return *error = std::string(field) + " is required", false;
  if (v.size() > max) return *error = std::string(field) + " is " + std::to_string(v.size()) + " bytes; at most " + std::to_string(max), false;
  for (unsigned char c : v) {
    if (c < 0x20 || c == 0x7f) return *error = std::string(field) + " must be one line of text", false;
  }
  if (base::valid_utf8(v) != v) return *error = std::string(field) + " isn't UTF-8", false;
  return true;
}

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

int64_t int_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_int64() ? v->as_int64() : 0;
}

void count_words(std::map<std::string, int, std::less<>>& into, std::string_view text) {
  for (auto& w : words_of(text)) ++into[w];
}

// How often words starting with `term` occur in a field, counted no further than `cap`.
int prefix_count(const std::map<std::string, int, std::less<>>& field, const std::string& term,
                 int cap = std::numeric_limits<int>::max()) {
  int n = 0;
  for (auto it = field.lower_bound(term); it != field.end() && it->first.starts_with(term) && n < cap; ++it) n += it->second;
  return std::min(n, cap);
}

bool is_word_byte(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80; }

// The body's first line holding a word that starts with one of the terms, cut to about 200 bytes
// around it at spaces; or the body's start.
std::string snippet(std::string_view body, const std::vector<std::string>& terms) {
  constexpr size_t kMax = 200;
  auto cut = [&](std::string_view line, size_t at) {
    size_t from = at > kMax / 3 ? at - kMax / 3 : 0;
    while (from > 0 && line[from - 1] != ' ') --from;  // a whole word (or the line's start)
    std::string_view s = line.substr(from);
    std::string out = from > 0 ? "…" : "";
    if (s.size() > kMax) {
      size_t end = kMax;
      while (end > 0 && s[end] != ' ') --end;
      if (end == 0) end = kMax;
      out += std::string(s.substr(0, end)) + "…";
    } else {
      out += std::string(s);
    }
    return base::valid_utf8(trim(out));
  };
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    std::string_view line = body.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    pos = end == std::string_view::npos ? body.size() : end + 1;
    std::string lower = base::to_lower(line);
    for (const auto& t : terms) {
      for (size_t at = lower.find(t); at != std::string::npos; at = lower.find(t, at + 1)) {
        if (at == 0 || !is_word_byte(static_cast<unsigned char>(lower[at - 1]))) return cut(line, at);
      }
    }
  }
  // No match in the body (the name or description matched): its first line of text.
  pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    std::string line = trim(body.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
    pos = end == std::string_view::npos ? body.size() : end + 1;
    if (!line.empty() && line.front() != '#' && !line.starts_with("```")) return cut(line, 0);
  }
  return "";
}

}  // namespace

std::vector<std::string> words_of(std::string_view text) {
  std::vector<std::string> out;
  std::string w;
  for (unsigned char c : text) {
    if (is_word_byte(c)) {
      w += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : char(c);
    } else if (!w.empty()) {
      out.push_back(std::move(w));
      w.clear();
    }
  }
  if (!w.empty()) out.push_back(std::move(w));
  return out;
}

std::optional<std::string> name_refusal(std::string_view name) {
  if (name.empty()) return "name is required";
  if (name.size() > kNameMaxBytes) return "name is at most " + std::to_string(kNameMaxBytes) + " bytes";
  if (!(name.front() >= 'a' && name.front() <= 'z')) return "name must start with a lowercase letter";
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (!lower_or_digit(c) && c != '-') return "name is lowercase letters, digits, and hyphens, like deploy-a-box";
    if (c == '-' && (i + 1 == name.size() || name[i + 1] == '-')) return "name has no \"--\" and doesn't end in a hyphen";
  }
  return std::nullopt;
}

bool check(Skill& s, std::string* error) {
  if (auto why = name_refusal(s.name)) return *error = *why, false;
  if (!one_line(s.title, "title", kTitleMaxBytes, error)) return false;
  if (!one_line(s.description, "description", kDescriptionMaxBytes, error)) return false;
  if (s.kind.empty()) s.kind = "skill";
  if (std::find(std::begin(kKinds), std::end(kKinds), s.kind) == std::end(kKinds)) {
    return *error = "kind is \"skill\" or \"playbook\"", false;
  }
  std::set<std::string> roles;
  for (const auto& r : s.roles) {
    std::string role = base::to_lower(trim(r));
    if (role.empty() || role.size() > kRoleMaxBytes || !(role.front() >= 'a' && role.front() <= 'z') || role.back() == '-' ||
        !std::all_of(role.begin(), role.end(), [](char c) { return lower_or_digit(c) || c == '-'; })) {
      return *error = "a role is 1 to " + std::to_string(kRoleMaxBytes) +
                      " lowercase letters, digits, and hyphens, starting with a letter, like box-engineer: \"" +
                      base::valid_utf8(r.substr(0, 60)) + "\" isn't",
             false;
    }
    roles.insert(std::move(role));
  }
  if (roles.empty()) return *error = "roles is required: at least one, like [\"box-engineer\"], so a reader finds it", false;
  if (roles.size() > kRolesMax) return *error = "at most " + std::to_string(kRolesMax) + " roles", false;
  s.roles.assign(roles.begin(), roles.end());
  std::string body;
  body.reserve(s.body.size());
  for (size_t i = 0; i < s.body.size(); ++i) {
    if (s.body[i] == '\r') {
      body += '\n';
      if (i + 1 < s.body.size() && s.body[i + 1] == '\n') ++i;
    } else {
      body += s.body[i];
    }
  }
  body = trim(body);
  if (body.empty()) return *error = "body is required: the skill itself, in markdown", false;
  if (body.size() > kBodyMaxBytes) {
    return *error = "body is " + std::to_string(body.size()) + " bytes; at most " + std::to_string(kBodyMaxBytes) +
                    " (split it into skills that name each other)",
           false;
  }
  if (body.find('\0') != std::string::npos || base::valid_utf8(body) != body) return *error = "body must be UTF-8 text", false;
  s.body = std::move(body) + "\n";
  return true;
}

std::string to_file(const Skill& s) {
  bj::array roles;
  for (const auto& r : s.roles) roles.push_back(bj::string(r));
  std::string out = "---\n";
  out += "name: " + s.name + "\n";
  out += "description: " + json::dump(bj::string(s.description)) + "\n";
  out += "title: " + json::dump(bj::string(s.title)) + "\n";
  out += "kind: " + s.kind + "\n";
  out += "roles: " + json::dump(roles) + "\n";
  out += "revision: " + std::to_string(s.revision) + "\n";
  out += "created-ms: " + std::to_string(s.created_ms) + "\n";
  out += "created-by: " + json::dump(bj::string(s.created_by)) + "\n";
  out += "updated-ms: " + std::to_string(s.updated_ms) + "\n";
  out += "updated-by: " + json::dump(bj::string(s.updated_by)) + "\n";
  out += "---\n\n";
  out += s.body;
  return out;
}

std::optional<Skill> from_file(std::string_view text, std::string_view expected_name, std::string* error) {
  std::string err;
  auto doc = render::split_front_matter(text, &err);
  if (!err.empty() || !doc.has_front_matter) return *error = err.empty() ? "no front matter" : err, std::nullopt;
  const auto& f = doc.fields;
  Skill s;
  s.name = text_of(f, "name");
  if (s.name != expected_name) return *error = "its name isn't its file's", std::nullopt;
  s.title = text_of(f, "title");
  s.description = text_of(f, "description");
  s.kind = text_of(f, "kind");
  if (const auto* r = f.if_contains("roles"); r && r->is_array()) {
    for (const auto& v : r->as_array()) {
      if (v.is_string()) s.roles.emplace_back(v.as_string());
    }
  }
  s.body = doc.body;
  s.revision = int_of(f, "revision");
  s.created_ms = int_of(f, "created-ms");
  s.updated_ms = int_of(f, "updated-ms");
  s.created_by = text_of(f, "created-by");
  s.updated_by = text_of(f, "updated-by");
  if (!check(s, error)) return std::nullopt;
  return s;
}

std::string etag_of(std::string_view file_text) { return "\"" + base::sha256_hex(file_text).substr(0, 16) + "\""; }

Library::Entry Library::index(Found found) {
  Entry e;
  const Skill& s = found.skill;
  count_words(e.head, s.name);
  count_words(e.head, s.title);
  for (const auto& r : s.roles) count_words(e.roles, r);
  count_words(e.description, s.description);
  count_words(e.body, s.body);
  e.found = std::move(found);
  return e;
}

void Library::put(Skill skill, int64_t file_bytes, std::string etag) {
  std::string name = skill.name;
  Entry e = index(Found{std::move(skill), std::move(etag), file_bytes});
  std::unique_lock lock(mu_);
  if (auto it = skills_.find(name); it != skills_.end()) {
    bytes_ -= it->second.found.bytes;
    it->second = std::move(e);
  } else {
    skills_.emplace(std::move(name), std::move(e));
  }
  bytes_ += file_bytes;
  count_ = skills_.size();
}

void Library::remove(std::string_view name) {
  std::unique_lock lock(mu_);
  if (auto it = skills_.find(name); it != skills_.end()) {
    bytes_ -= it->second.found.bytes;
    skills_.erase(it);
  }
  count_ = skills_.size();
}

void Library::clear() {
  std::unique_lock lock(mu_);
  skills_.clear();
  bytes_ = 0;
  count_ = 0;
}

std::optional<Library::Found> Library::get(std::string_view name) const {
  std::shared_lock lock(mu_);
  auto it = skills_.find(name);
  if (it == skills_.end()) return std::nullopt;
  return it->second.found;
}

size_t Library::count() const { return count_.load(); }

int64_t Library::bytes() const { return bytes_.load(); }

bj::object Library::card(const Found& f) {
  const Skill& s = f.skill;
  bj::array roles;
  for (const auto& r : s.roles) roles.push_back(bj::string(r));
  return bj::object{{"name", s.name},           {"title", s.title},           {"kind", s.kind},
                    {"roles", std::move(roles)}, {"description", s.description}, {"revision", s.revision},
                    {"updated_ms", s.updated_ms}, {"updated_by", s.updated_by},   {"bytes", f.bytes}};
}

bj::array Library::list(std::string_view role, std::string_view kind) const {
  std::shared_lock lock(mu_);
  bj::array out;
  for (const auto& [name, e] : skills_) {
    const Skill& s = e.found.skill;
    if (!kind.empty() && s.kind != kind) continue;
    if (!role.empty() && std::find(s.roles.begin(), s.roles.end(), role) == s.roles.end()) continue;
    out.push_back(card(e.found));
  }
  return out;
}

bj::array Library::roles() const {
  std::shared_lock lock(mu_);
  std::map<std::string, std::map<std::string, int64_t>> by;
  for (const auto& [name, e] : skills_) {
    for (const auto& r : e.found.skill.roles) ++by[r][e.found.skill.kind];
  }
  bj::array out;
  for (const auto& [role, kinds] : by) {
    int64_t n = 0;
    bj::object k;
    for (const auto& [kind, c] : kinds) {
      k[kind] = c;
      n += c;
    }
    out.push_back(bj::object{{"role", role}, {"skills", n}, {"kinds", std::move(k)}});
  }
  return out;
}

bj::array Library::search(std::string_view query, std::string_view role, std::string_view kind, int64_t limit) const {
  std::vector<std::string> terms;
  for (auto& w : words_of(query)) {
    if (std::find(terms.begin(), terms.end(), w) == terms.end()) terms.push_back(std::move(w));
    if (terms.size() == kQueryTermsMax) break;
  }
  limit = std::clamp<int64_t>(limit, 1, kSearchLimitMax);
  struct Hit {
    int64_t score;
    const Entry* e;
  };
  std::vector<Hit> hits;
  std::shared_lock lock(mu_);
  for (const auto& [name, e] : skills_) {
    const Skill& s = e.found.skill;
    if (!kind.empty() && s.kind != kind) continue;
    if (!role.empty() && std::find(s.roles.begin(), s.roles.end(), role) == s.roles.end()) continue;
    int64_t score = 0;
    bool every = true;
    for (const auto& t : terms) {
      int64_t here = 8 * prefix_count(e.head, t) + 5 * prefix_count(e.roles, t) + 3 * prefix_count(e.description, t) +
                     prefix_count(e.body, t, 5);
      if (here == 0) {
        every = false;
        break;
      }
      score += here;
    }
    if (every) hits.push_back({score, &e});
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
  bj::array out;
  for (const auto& h : hits) {
    if (int64_t(out.size()) == limit) break;
    bj::object c = card(h.e->found);
    c["score"] = h.score;
    c["snippet"] = snippet(h.e->found.skill.body, terms);
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace goober::skills

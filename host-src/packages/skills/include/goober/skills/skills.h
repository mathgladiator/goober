#pragma once

// Skills and playbooks (design/machine/050.Skills.md): what the host's engineers know how to do,
// written down by operators so the next engineer starts from it. A skill is one markdown file,
// skills/$name.md, whose front matter is an Agent Skills SKILL.md's (name, description) plus the
// host's own fields; this package checks one, reads and writes the file, and keeps the library
// every tool searches.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::skills {

namespace bj = boost::json;

inline constexpr size_t kNameMaxBytes = 64;
inline constexpr size_t kTitleMaxBytes = 120;
inline constexpr size_t kDescriptionMaxBytes = 1024;
inline constexpr size_t kRoleMaxBytes = 40;
inline constexpr size_t kRolesMax = 8;
inline constexpr size_t kBodyMaxBytes = 64 * 1024;
inline constexpr size_t kSkillsMax = 1000;
inline constexpr size_t kQueryTermsMax = 16;
inline constexpr int64_t kSearchLimitMax = 50;

// "skill": how to do one thing well. "playbook": what to do, step by step, when something happens.
inline constexpr std::string_view kKinds[] = {"skill", "playbook"};

struct Skill {
  std::string name;  // lowercase letters, digits, and single hyphens; the file's name
  std::string title;
  std::string description;  // when to reach for it, one line
  std::string kind = "skill";
  std::vector<std::string> roles;  // who it is for, like "box-engineer"; sorted, no repeats
  std::string body;                // markdown
  int64_t revision = 0;
  int64_t created_ms = 0, updated_ms = 0;
  std::string created_by, updated_by;
};

// Why a name can't be a skill's, or nullopt. The Agent Skills rule: 1 to 64 bytes of lowercase
// letters, digits, and hyphens, starting with a letter, with no "--" and no hyphen at the end.
std::optional<std::string> name_refusal(std::string_view name);

// Checks what a put supplies and puts it in one form: one-line fields trimmed, roles lowercased,
// sorted, without repeats, the body's line ends as "\n". False with `error` naming the field.
bool check(Skill& skill, std::string* error);

// The file: front matter (name, description, then the host's fields), a blank line, the body.
std::string to_file(const Skill& skill);
// Reads a file to_file wrote, checking it as a put is checked; `expected_name` is the file's.
std::optional<Skill> from_file(std::string_view text, std::string_view expected_name, std::string* error);

// A file's etag, as if_etag compares it: the first 16 hex digits of its SHA-256, in quotes.
std::string etag_of(std::string_view file_text);

// Every skill the host keeps, indexed for search. Safe from any thread.
class Library {
 public:
  // Puts (or replaces) one skill, as its file of `file_bytes` bytes and `etag`.
  void put(Skill skill, int64_t file_bytes, std::string etag);
  void remove(std::string_view name);
  void clear();

  struct Found {
    Skill skill;
    std::string etag;
    int64_t bytes = 0;
  };
  std::optional<Found> get(std::string_view name) const;

  size_t count() const;
  int64_t bytes() const;  // every file's bytes

  // Each skill's card (no body), by name; only a role's or a kind's when given.
  bj::array list(std::string_view role, std::string_view kind) const;
  // Every role named, with how many skills name it (and of which kind), by name.
  bj::array roles() const;
  // The skills matching every word of `query` (a word matches a word that starts with it), best
  // first: name and title count most, then roles, the description, and the body. Each with a
  // snippet of the body around the first match.
  bj::array search(std::string_view query, std::string_view role, std::string_view kind, int64_t limit) const;

 private:
  struct Entry {
    Found found;
    // Each field's words, lowercased: a word's count in it.
    std::map<std::string, int, std::less<>> head, roles, description, body;
  };
  static Entry index(Found found);
  static bj::object card(const Found& f);

  mutable std::mutex mu_;
  std::map<std::string, Entry, std::less<>> skills_;
  int64_t bytes_ = 0;
};

// The words of a text as search reads them: runs of letters and digits, lowercased (bytes past
// ASCII are kept as part of a word, so any language's words are words).
std::vector<std::string> words_of(std::string_view text);

}  // namespace goober::skills

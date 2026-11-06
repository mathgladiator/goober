#pragma once

// A mailbox on disk: one directory, a message per file named by its id (13 digits of
// milliseconds, "-", 8 hex digits, so names sort by time), and beside each a small `.head` row a
// list reads instead of the whole message. Beta's mailboxes (053 § 4) and the operators'
// (055) keep their messages this way.

#include <boost/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace goober::host::mailbox {

// A fresh id for a message kept at `now_ms`.
std::string new_id(int64_t now_ms);
bool valid_id(std::string_view id);
// The first `max` bytes of a text, cut back to a UTF-8 boundary, so what is kept stays text.
std::string cut_utf8(std::string_view s, size_t max);

// Writes a message (durably) and its row (not: a lost row is rebuilt from the message).
bool write(const std::filesystem::path& dir, const std::string& id, const boost::json::object& message,
           const boost::json::object& head);

// Drops the oldest messages past `max_count`, past `max_age_ms`, or past `budget` bytes in all;
// the newest one stays whatever its size.
void trim(const std::filesystem::path& dir, size_t max_count, int64_t max_age_ms, int64_t budget, int64_t now_ms);

// Rows newest first, at most `limit` of them that `keep` takes; a message without a readable row
// gives the row `head_of` makes from it.
using HeadOf = std::function<boost::json::object(const boost::json::object& message)>;
using Keep = std::function<bool(const boost::json::object& head)>;
boost::json::array list(const std::filesystem::path& dir, int64_t limit, const HeadOf& head_of, const Keep& keep = {});

std::optional<boost::json::object> get(const std::filesystem::path& dir, const std::string& id);
// Removes one message, its row, and its parts directory. False when there was none.
bool remove(const std::filesystem::path& dir, const std::string& id);
// Removes every message kept before `before_ms` (every message when 0). Answers how many.
int64_t remove_before(const std::filesystem::path& dir, int64_t before_ms);
// The bytes the directory holds.
int64_t bytes(const std::filesystem::path& dir);

}  // namespace goober::host::mailbox

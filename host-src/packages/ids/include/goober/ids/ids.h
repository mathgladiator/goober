#pragma once

// The host's id service (design/human/021.PrimaryKeys.md; design/machine/038.Ids.md § 2): one
// counter that only moves forward, ranges vended from it durably before use, and ids taken
// from the current range. Every id it mints is unique across every table, database, realm,
// and tenant on the host.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace goober::sqlite {
class Db;
}

namespace goober::ids {

// Where a fresh service starts: 2^32 + 1, above every SQLite rowid and AUTOINCREMENT value a
// host made before it, so an old id and a host id are never the same number, and past every
// 32-bit integer, so any code that would truncate an id fails from the first one (038 § 2).
inline constexpr int64_t kFirstId = (int64_t(1) << 32) + 1;
// How far past the highest id already stored the counter jumps, at first start and after a
// restore, so an id written after the last backup is never minted again (021).
inline constexpr int64_t kMargin = 10'000'000;
inline constexpr int64_t kRangeLength = 10'000;

struct Range {
  int64_t start = 0, length = 0;
  std::string vended_to;
  int64_t vended_ms = 0;
};

class Service {
 public:
  // Opens (or makes) the counter in `file`, named `host` in the ranges it takes. A new
  // counter starts at max(kFirstId, floor), where floor is the first id the caller knows is
  // free (the highest id already stored plus kMargin); 0 for none.
  Service(const std::filesystem::path& file, std::string host, int64_t floor = 0);
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // The next id. Thread-safe; takes a new range when the current one is spent.
  int64_t next();

  // Moves the counter to at least `floor` (durably) and drops the rest of the current range,
  // so every later id is >= floor. For a restore: floor = the highest id restored + kMargin.
  void move_past(int64_t floor);

  // The first id never vended.
  int64_t counter();
  // The range in use: [next, end), or end 0 before the first id.
  std::pair<int64_t, int64_t> current();
  // Newest first.
  std::vector<Range> ranges(int limit);

  // Whether the counter was made by this open (a first start).
  bool created() const { return created_; }

 private:
  void take_range();
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
  std::string host_;
  int64_t next_ = 0, end_ = 0;
  bool created_ = false;
};

// The highest id one SQLite file holds: the largest value of every table's integer primary
// key, and of sqlite_sequence (AUTOINCREMENT's high-water mark, deleted rows included). 0 for
// none, or for a file that isn't a database.
int64_t highest_id(const std::filesystem::path& file);
// The highest over every *.db file under `dir`, recursively, skipping `skip` (the counter's
// own file).
int64_t highest_id_under(const std::filesystem::path& dir, const std::filesystem::path& skip = {});

}  // namespace goober::ids

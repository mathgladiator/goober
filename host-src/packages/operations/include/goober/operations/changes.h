#pragma once

// The change log for the operator panel (design/machine/028.Panel.md § 5): one row for each
// meaningful change in a host WAL bundle, written by the WAL's observer while the old files
// are still on disk, and one row for each operator tool call. Kept in
// storage/operations/changes.db.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "goober/json/json.h"
#include "goober/wal/wal.h"

namespace goober::sqlite {
class Db;
}

namespace goober::operations {

namespace bj = boost::json;

// What a WAL path is, for the change log.
struct PathKind {
  std::string kind;    // content, partial, template, schema, attachment, box, other; "" to skip
  std::string tenant;  // the tree or tenant, or "" (schemas are host-wide)
  std::string target;  // the path in the tree, the box, the schema, or the attachment id
  std::string version; // for a box path under versions/, the version
  bool active = false; // boxes/$box/active
};
PathKind classify_path(std::string_view path);

struct Action {
  int64_t at_ms = 0;
  std::string operator_name, tool, request_id;
  bj::object args;  // the access log's arg_* fields, without the prefix
  bool failed = false;
};

class ChangeLog {
 public:
  // Texts of at most this many bytes are kept with a row, for diffs.
  static constexpr size_t kTextMaxBytes = 64 * 1024;

  ChangeLog(std::filesystem::path db_file, std::filesystem::path storage_root);
  ~ChangeLog();
  ChangeLog(const ChangeLog&) = delete;
  ChangeLog& operator=(const ChangeLog&) = delete;

  // Writes the rows for one bundle. The bundle's files must not have been applied yet.
  void record(const wal::Bundle& bundle, const std::string& actor, const std::string& request_id, int64_t now_ms);
  void record_action(const Action& action);

  // Newest first, without texts: [{id, at_ms, kind, action, tenant, target, actor, request_id,
  // bytes_before, bytes_after, hash_before, hash_after, has_text, detail}].
  bj::array list(const std::string& kind, const std::string& tenant, int64_t before_id, int64_t limit);
  // One row, with "before" and "after" when kept.
  std::optional<bj::object> get(int64_t id);
  // Newest first: [{id, at_ms, operator, tool, request_id, args, failed, changes: [ids]}].
  bj::array actions(bool changed_only, int64_t before_id, int64_t limit);

 private:
  void insert(int64_t now_ms, const std::string& kind, const std::string& action, const std::string& tenant,
              const std::string& target, const std::string& actor, const std::string& request_id,
              const std::optional<std::string>& before, const std::optional<std::string>& after, bool keep_text,
              const bj::object& detail);
  std::optional<std::string> read_old(std::string_view path, uint64_t* size) const;

  std::filesystem::path root_;
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
  int64_t last_trim_ms_ = 0;
};

}  // namespace goober::operations

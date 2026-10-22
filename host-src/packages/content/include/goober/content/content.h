#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/wal/wal.h"

namespace goober::content {

// The tree that holds host-wide content, like error pages. Its leading "_"
// keeps it apart from every tenant name.
inline constexpr std::string_view kHostTree = "_host";

struct File {
  std::string data;
  std::string content_type;
  std::string etag;  // quoted, the first 16 hex digits of the SHA-256
};

// The content type for a path, from its extension.
std::string content_type_for(std::string_view path);

// Static content per tenant, stored under storage/content/$tenant/$path and
// changed only through the host WAL.
class ContentStore {
 public:
  explicit ContentStore(wal::HostWal& wal);

  // `tree` is a tenant name or kHostTree; `path` is a safe relative path.
  bool put(std::string_view tree, std::string_view path, std::string_view data, std::string* error);
  bool remove(std::string_view tree, std::string_view path, std::string* error);
  std::optional<File> get(std::string_view tree, std::string_view path) const;
  std::vector<std::string> list(std::string_view tree, std::string_view prefix) const;

  // Resolves a normalized URL path ("/", "/a/b") to a file: "/" serves
  // index.html; "/a" serves a, or else a/index.html.
  std::optional<File> resolve(std::string_view tree, std::string_view url_path) const;

 private:
  bool valid(std::string_view tree, std::string_view path, std::string* error) const;
  wal::HostWal& wal_;
};

}  // namespace goober::content

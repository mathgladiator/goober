#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/content/content.h"
#include "goober/wal/wal.h"

namespace goober::attachments {

// A type an attachment may have, detected from its bytes (design/machine/007.Content.md §7).
struct Sniffed {
  std::string content_type;
  std::string ext;
  bool inline_ok = false;  // raster images show inline; SVG and PDF are downloads
};

// The type of the bytes, or nullopt when they aren't PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, or PDF.
std::optional<Sniffed> sniff(std::string_view bytes);

// Whether the bytes run to the end of their format: PNG's IEND chunk, JPEG's end-of-image
// marker, GIF's trailer, WebP's RIFF length, PDF's %%EOF, an SVG's closing tag. A file cut
// off on its way (a truncated base64 argument, say) has the right start and no end. AVIF and
// ICO aren't checked. On false, `why` says what is missing.
bool looks_complete(std::string_view bytes, const Sniffed& type, std::string* why);

struct Attachment {
  int64_t id = 0;
  std::string state;  // writing, stored, failed, or deleted
  std::string content_type;
  std::string ext;
  int64_t size = 0;
  std::string sha256;
  std::string name;  // what the uploader called it
  std::string alt;
  std::string visibility;  // public or members
  int64_t created_ms = 0;
  std::string url() const { return "/attachment/" + std::to_string(id) + "." + ext; }
  bool inline_ok() const;
};

// Attachments per tenant: a ledger in storage/sqlite3/$tenant/internal.db, and bytes at
// storage/objects/$tenant/attachments/$id written through the host WAL. Thread-safe;
// each call opens the ledger it needs.
class Store {
 public:
  explicit Store(wal::HostWal& wal) : wal_(wal) {}

  // Stores bytes as a new attachment. The ledger row is written before the bytes, so no
  // file exists that the ledger doesn't know (060.Files.md § The upload ledger).
  std::optional<Attachment> put(std::string_view tenant, std::string_view bytes, std::string_view name,
                                std::string_view alt, std::string_view visibility, int64_t now_ms, std::string* error);
  // A stored attachment, or nullopt.
  std::optional<Attachment> get(std::string_view tenant, int64_t id) const;
  std::optional<std::string> read(std::string_view tenant, int64_t id) const;
  // Every stored attachment, newest first.
  std::vector<Attachment> list(std::string_view tenant) const;
  bool remove(std::string_view tenant, int64_t id, int64_t now_ms, std::string* error);

 private:
  wal::HostWal& wal_;
};

// Where each attachment is used: the content paths whose text holds /attachment/$id.
std::map<int64_t, std::vector<std::string>> references(const content::ContentStore& content, std::string_view tenant);

}  // namespace goober::attachments

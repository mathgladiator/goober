// Page-level deltas of SQLite files. A snapshot (from SQLite's online backup) is compared
// page by page with the copy in the night's archive; only differing pages are kept.

#include <cstring>
#include <fstream>

#include "goober/backup/backup.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/json/json.h"

namespace goober::backup {
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

std::array<uint8_t, 16> page_hash(std::string_view page) {
  auto full = base::sha256(page);
  std::array<uint8_t, 16> out{};
  std::memcpy(out.data(), full.data(), out.size());
  return out;
}

// The page size a SQLite file declares, or 0 for an empty file; nullopt when it isn't one.
std::optional<uint32_t> declared_page_size(std::ifstream& in, uint64_t size) {
  if (size == 0) return 0;
  char header[100];
  in.read(header, sizeof header);
  if (!in || std::memcmp(header, "SQLite format 3\0", 16) != 0) return std::nullopt;
  uint32_t ps = (uint32_t(uint8_t(header[16])) << 8) | uint8_t(header[17]);
  if (ps == 1) ps = 65536;
  if (ps < 512 || (ps & (ps - 1)) != 0) return std::nullopt;
  in.seekg(0);
  return ps;
}

}  // namespace

std::optional<PageMap> map_pages(const fs::path& db, std::string* error) {
  std::error_code ec;
  uint64_t size = fs::file_size(db, ec);
  if (ec) return *error = "can't read " + db.string(), std::nullopt;
  std::ifstream in(db, std::ios::binary);
  auto ps = declared_page_size(in, size);
  if (!ps) return *error = db.filename().string() + " isn't a SQLite database", std::nullopt;
  PageMap map;
  map.page_size = *ps ? *ps : 4096;
  map.pages = (size + map.page_size - 1) / map.page_size;
  base::Sha256 whole;
  std::string page(map.page_size, '\0');
  for (uint64_t i = 0; i < map.pages; ++i) {
    in.read(page.data(), std::streamsize(page.size()));
    size_t n = size_t(in.gcount());
    std::string_view got(page.data(), n);
    whole.update(got);
    map.hashes.push_back(page_hash(got));
  }
  auto digest = whole.finish();
  map.sha256 = base::hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
  return map;
}

bool write_delta(const PageMap* base, const fs::path& snapshot, const fs::path& out_path, DeltaInfo* info,
                 std::string* error) {
  auto snap = map_pages(snapshot, error);
  if (!snap) return false;
  std::error_code ec;
  uint64_t bytes = fs::file_size(snapshot, ec);
  // A changed page size makes every page new.
  bool comparable = base && base->page_size == snap->page_size;
  std::ifstream in(snapshot, std::ios::binary);
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!in || !out) return *error = "can't write the delta for " + snapshot.filename().string(), false;
  std::string header = json::dump(bj::object{{"format", 1},
                                             {"page_size", snap->page_size},
                                             {"pages", snap->pages},
                                             {"bytes", bytes},
                                             {"sha256", snap->sha256}}) +
                       "\n";
  out.write(header.data(), std::streamsize(header.size()));
  std::string page(snap->page_size, '\0');
  uint64_t changed = 0;
  for (uint64_t i = 0; i < snap->pages; ++i) {
    in.read(page.data(), std::streamsize(page.size()));
    size_t n = size_t(in.gcount());
    bool same = comparable && i < base->hashes.size() && base->hashes[i] == snap->hashes[i];
    if (same) continue;
    uint8_t no[4] = {uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)};
    out.write(reinterpret_cast<char*>(no), 4);
    // A short last page is padded; the header's byte count cuts it back.
    std::memset(page.data() + n, 0, page.size() - n);
    out.write(page.data(), std::streamsize(page.size()));
    ++changed;
  }
  out.flush();
  if (!out) return *error = "writing the delta for " + snapshot.filename().string() + " failed", false;
  info->pages = snap->pages;
  info->changed = changed;
  info->sha256 = snap->sha256;
  return true;
}

bool apply_delta(const fs::path* base, const fs::path& delta, const fs::path& out_path, std::string* error) {
  std::ifstream in(delta, std::ios::binary);
  std::string header;
  if (!in || !std::getline(in, header)) return *error = "can't read " + delta.string(), false;
  std::string err;
  auto h = json::parse(header, &err);
  if (!h || !h->is_object() || !h->as_object().contains("page_size") || !h->as_object().contains("bytes")) {
    return *error = delta.filename().string() + " isn't a page delta", false;
  }
  uint64_t page_size = h->at("page_size").to_number<uint64_t>();
  uint64_t bytes = h->at("bytes").to_number<uint64_t>();
  std::string sha = std::string(h->at("sha256").as_string());
  if (page_size < 512 || page_size > 65536) return *error = delta.filename().string() + " has a bad page size", false;
  fs::path tmp = out_path.string() + ".applying";
  std::error_code ec;
  if (base && fs::exists(*base)) {
    fs::copy_file(*base, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return *error = "can't copy " + base->string() + ": " + ec.message(), false;
  } else {
    std::ofstream(tmp, std::ios::binary | std::ios::trunc);
  }
  {
    std::fstream out(tmp, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return *error = "can't write " + tmp.string(), false;
    std::string page(page_size, '\0');
    uint8_t no[4];
    while (in.read(reinterpret_cast<char*>(no), 4)) {
      uint64_t i = (uint64_t(no[0]) << 24) | (uint64_t(no[1]) << 16) | (uint64_t(no[2]) << 8) | no[3];
      if (!in.read(page.data(), std::streamsize(page.size()))) return *error = delta.filename().string() + " is cut short", false;
      if (i * page_size >= bytes) return *error = delta.filename().string() + " writes past its end", false;
      out.seekp(std::streamoff(i * page_size));
      out.write(page.data(), std::streamsize(page.size()));
    }
    if (!out) return *error = "writing " + tmp.string() + " failed", false;
  }
  fs::resize_file(tmp, bytes, ec);
  if (ec) return *error = "can't size " + tmp.string() + ": " + ec.message(), false;
  auto got = base::sha256_file_hex(tmp);
  if (!got || *got != sha) {
    fs::remove(tmp, ec);
    return *error = "rebuilding from " + delta.filename().string() + " gave the wrong bytes (its base isn't the right night?)", false;
  }
  fs::rename(tmp, out_path, ec);
  if (ec) return *error = "can't move " + out_path.string() + " into place: " + ec.message(), false;
  return true;
}

}  // namespace goober::backup

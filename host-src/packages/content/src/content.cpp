#include "goober/content/content.h"

#include <algorithm>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"

namespace goober::content {

std::string content_type_for(std::string_view path) {
  auto dot = path.rfind('.');
  auto slash = path.rfind('/');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return "application/octet-stream";
  }
  std::string ext = base::to_lower(path.substr(dot + 1));
  static const std::pair<const char*, const char*> kTypes[] = {
      {"html", "text/html; charset=utf-8"},  {"htm", "text/html; charset=utf-8"},
      {"css", "text/css; charset=utf-8"},    {"js", "text/javascript; charset=utf-8"},
      {"mjs", "text/javascript; charset=utf-8"}, {"json", "application/json"},
      {"txt", "text/plain; charset=utf-8"},  {"md", "text/markdown; charset=utf-8"},
      {"svg", "image/svg+xml"},              {"png", "image/png"},
      {"jpg", "image/jpeg"},                 {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},                  {"webp", "image/webp"},
      {"ico", "image/x-icon"},               {"wasm", "application/wasm"},
      {"woff", "font/woff"},                 {"woff2", "font/woff2"},
      {"xml", "application/xml"},            {"pdf", "application/pdf"},
      {"mp3", "audio/mpeg"},                 {"mp4", "video/mp4"},
      {"ogg", "audio/ogg"},                  {"wav", "audio/wav"},
      {"csv", "text/csv; charset=utf-8"},    {"tsv", "text/tab-separated-values; charset=utf-8"},
      {"atom", "application/atom+xml; charset=utf-8"}, {"rss", "application/rss+xml; charset=utf-8"},
      {"webmanifest", "application/manifest+json"}, {"ics", "text/calendar; charset=utf-8"},
      {"avif", "image/avif"},                {"webm", "video/webm"},
      {"geojson", "application/geo+json"},   {"yaml", "text/yaml; charset=utf-8"},
      {"yml", "text/yaml; charset=utf-8"},   {"map", "application/json"},
  };
  for (const auto& [e, t] : kTypes) {
    if (ext == e) return t;
  }
  return "application/octet-stream";
}

namespace {
thread_local const Overlay* t_overlay = nullptr;

const std::optional<std::string>* overlaid(std::string_view tree, std::string_view path) {
  if (!t_overlay || t_overlay->tree != tree) return nullptr;
  auto it = t_overlay->files.find(std::string(path));
  return it == t_overlay->files.end() ? nullptr : &it->second;
}
}  // namespace

ContentStore::Preview::Preview(const Overlay* overlay) : previous_(t_overlay) { t_overlay = overlay; }
ContentStore::Preview::~Preview() { t_overlay = previous_; }

ContentStore::ContentStore(wal::HostWal& wal) : wal_(wal) {}

bool ContentStore::valid(std::string_view tree, std::string_view path, std::string* error) const {
  if (tree != kHostTree && !base::is_valid_name(tree)) {
    if (error) *error = "tenant must match [a-z][a-z0-9_]*";
    return false;
  }
  if (!base::is_safe_relative_path(path)) {
    if (error) *error = "path must be relative, with no empty, \".\", or \"..\" segments: " + std::string(path);
    return false;
  }
  return true;
}

bool ContentStore::put(std::string_view tree, std::string_view path, std::string_view data, std::string* error) {
  if (!valid(tree, path, error)) return false;
  std::string rel = "content/" + std::string(tree) + "/" + std::string(path);
  return wal_.commit({{wal::FileChange::Kind::kWrite, rel, std::string(data)}}, error);
}

bool ContentStore::remove(std::string_view tree, std::string_view path, std::string* error) {
  if (!valid(tree, path, error)) return false;
  if (!get(tree, path)) {
    if (error) *error = "no such file: " + std::string(path);
    return false;
  }
  std::string rel = "content/" + std::string(tree) + "/" + std::string(path);
  return wal_.commit({{wal::FileChange::Kind::kDelete, rel, ""}}, error);
}

std::optional<File> ContentStore::get(std::string_view tree, std::string_view path) const {
  if (!valid(tree, path, nullptr)) return std::nullopt;
  std::optional<std::string> data;
  if (const auto* o = overlaid(tree, path)) {
    data = *o;
  } else {
    data = base::read_file(wal_.root() / "content" / std::string(tree) / std::string(path));
  }
  if (!data) return std::nullopt;
  File f;
  f.etag = "\"" + base::sha256_hex(*data).substr(0, 16) + "\"";
  f.content_type = content_type_for(path);
  f.data = std::move(*data);
  return f;
}

std::vector<std::string> ContentStore::list(std::string_view tree, std::string_view prefix) const {
  std::vector<std::string> out;
  if (tree != kHostTree && !base::is_valid_name(tree)) return out;
  for (auto& f : base::list_files(wal_.root() / "content" / std::string(tree), prefix)) {
    if (!overlaid(tree, f)) out.push_back(std::move(f));
  }
  if (t_overlay && t_overlay->tree == tree) {
    for (const auto& [f, data] : t_overlay->files) {
      if (data && f.starts_with(prefix)) out.push_back(f);
    }
    std::sort(out.begin(), out.end());
  }
  return out;
}

std::optional<File> ContentStore::resolve(std::string_view tree, std::string_view url_path) const {
  std::string rel(url_path);
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  if (rel.empty()) return get(tree, "index.html");
  if (auto f = get(tree, rel)) return f;
  return get(tree, rel + "/index.html");
}

}  // namespace goober::content

#include "goober/specs/specs.h"

#include <algorithm>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/json/json.h"

namespace goober::specs {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

const char* kVerbs[] = {"get", "put", "post", "delete", "options", "head", "patch", "trace", "query"};

std::string str(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

// A local $ref ("#/components/parameters/limit"), resolved once.
const bj::value* resolve(const bj::object& root, const bj::value& v) {
  if (!v.is_object()) return &v;
  auto* ref = v.as_object().if_contains("$ref");
  if (!ref || !ref->is_string()) return &v;
  std::string_view r = ref->as_string();
  if (!r.starts_with("#/")) return nullptr;
  const bj::value* at = nullptr;
  const bj::object* cur = &root;
  r.remove_prefix(2);
  while (true) {
    auto slash = r.find('/');
    std::string seg(r.substr(0, slash));
    // JSON Pointer escapes.
    for (size_t p = seg.find("~1"); p != std::string::npos; p = seg.find("~1")) seg.replace(p, 2, "/");
    for (size_t p = seg.find("~0"); p != std::string::npos; p = seg.find("~0")) seg.replace(p, 2, "~");
    at = cur->if_contains(seg);
    if (!at) return nullptr;
    if (slash == std::string_view::npos) return at;
    if (!at->is_object()) return nullptr;
    cur = &at->as_object();
    r.remove_prefix(slash + 1);
  }
}

void add_params(const bj::object& root, const bj::value* list, std::vector<Param>* out, std::vector<std::string>* body_types,
                const std::vector<std::string>& consumes) {
  if (!list || !list->is_array()) return;
  for (const auto& pv : list->as_array()) {
    const auto* p = resolve(root, pv);
    if (!p || !p->is_object()) continue;
    const auto& o = p->as_object();
    std::string in = str(o, "in"), name = str(o, "name");
    if (in == "body") {  // Swagger 2.0
      for (const auto& c : consumes.empty() ? std::vector<std::string>{"application/json"} : consumes) body_types->push_back(c);
      continue;
    }
    if (in == "formData") {
      bool file = str(o, "type") == "file";
      std::string t = file ? "multipart/form-data" : "application/x-www-form-urlencoded";
      bool declared = std::find(consumes.begin(), consumes.end(), "multipart/form-data") != consumes.end();
      if (declared) t = "multipart/form-data";
      if (std::find(body_types->begin(), body_types->end(), t) == body_types->end()) body_types->push_back(t);
      continue;
    }
    bool required = o.contains("required") && o.at("required").is_bool() && o.at("required").as_bool();
    // A parameter given again at the operation replaces the path-level one.
    auto same = std::find_if(out->begin(), out->end(), [&](const Param& x) { return x.name == name && x.in == in; });
    if (same != out->end()) {
      same->required = required;
    } else {
      out->push_back(Param{name, in, required});
    }
  }
}

std::vector<std::string> strings(const bj::object& o, const char* k) {
  std::vector<std::string> out;
  if (auto* v = o.if_contains(k); v && v->is_array()) {
    for (const auto& e : v->as_array()) {
      if (e.is_string()) out.emplace_back(e.as_string());
    }
  }
  return out;
}

}  // namespace

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 64 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

const Operation* Spec::find(std::string_view ref) const {
  auto sp = ref.find(' ');
  if (sp != std::string_view::npos) {
    std::string verb = base::to_lower(ref.substr(0, sp));
    std::string_view path = ref.substr(sp + 1);
    for (const auto& op : ops) {
      if (op.verb == verb && op.path == path) return &op;
    }
    return nullptr;
  }
  for (const auto& op : ops) {
    if (!op.id.empty() && op.id == ref) return &op;
  }
  return nullptr;
}

std::optional<Spec> parse(std::string_view text, std::string* error) {
  std::string err;
  auto v = json::parse(text, &err);
  if (!v || !v->is_object()) return *error = "a spec is a JSON object (convert YAML to JSON first): " + err, std::nullopt;
  const auto& root = v->as_object();
  Spec spec;
  std::string openapi = str(root, "openapi"), swagger = str(root, "swagger");
  if (openapi.starts_with("3.")) {
    spec.format = "openapi " + openapi;
  } else if (swagger == "2.0") {
    spec.format = "swagger 2.0";
  } else {
    return *error = "not OpenAPI 3.x or Swagger 2.0 (no \"openapi\": \"3...\" or \"swagger\": \"2.0\")", std::nullopt;
  }
  if (auto* info = root.if_contains("info"); info && info->is_object()) {
    spec.title = str(info->as_object(), "title");
    spec.version = str(info->as_object(), "version");
  }
  std::string base_path;
  std::vector<std::string> consumes = strings(root, "consumes");
  if (swagger == "2.0") {
    base_path = str(root, "basePath");
    if (base_path == "/") base_path.clear();
    std::string host = str(root, "host");
    auto schemes = strings(root, "schemes");
    if (!host.empty()) spec.server = (std::find(schemes.begin(), schemes.end(), "https") != schemes.end() || schemes.empty() ? "https://" : "http://") + host + base_path;
  } else if (auto* servers = root.if_contains("servers"); servers && servers->is_array() && !servers->as_array().empty()) {
    if (servers->as_array()[0].is_object()) {
      std::string url = str(servers->as_array()[0].as_object(), "url");
      if (url.starts_with("https://") || url.starts_with("http://")) spec.server = url;
    }
  }
  auto* paths = root.if_contains("paths");
  if (!paths || !paths->is_object()) return *error = "the spec has no paths", std::nullopt;
  for (const auto& [path, item_v] : paths->as_object()) {
    // Every { closes before the next, so a path parameter always has a name.
    int depth = 0;
    for (char ch : std::string_view(path)) {
      depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
      if (depth < 0 || depth > 1) break;
    }
    if (depth != 0 || !path.starts_with("/")) return *error = "the path " + std::string(path) + " isn't a path with balanced {}", std::nullopt;
    const auto* item_p = resolve(root, item_v);
    if (!item_p || !item_p->is_object()) continue;
    const auto& item = item_p->as_object();
    for (const char* verb : kVerbs) {
      auto* op_v = item.if_contains(verb);
      if (!op_v || !op_v->is_object()) continue;
      const auto& o = op_v->as_object();
      Operation op;
      op.id = str(o, "operationId");
      op.verb = verb;
      // As the spec spells it: Swagger's basePath (like a 3.x server's path) belongs in the
      // manifest's base, as in https://www.strava.com/api/v3.
      op.path = std::string(path);
      auto op_consumes = strings(o, "consumes");
      const auto& cons = op_consumes.empty() ? consumes : op_consumes;
      add_params(root, item.if_contains("parameters"), &op.params, &op.body_types, cons);
      add_params(root, o.if_contains("parameters"), &op.params, &op.body_types, cons);
      if (auto* rb = o.if_contains("requestBody")) {
        const auto* body = resolve(root, *rb);
        if (body && body->is_object()) {
          if (auto* content = body->as_object().if_contains("content"); content && content->is_object()) {
            for (const auto& kv : content->as_object()) op.body_types.emplace_back(kv.key());
          }
        }
      }
      spec.ops.push_back(std::move(op));
    }
  }
  if (spec.ops.empty()) return *error = "the spec declares no operations", std::nullopt;
  return spec;
}

Store::Store(fs::path dir) : dir_(std::move(dir)) {}

std::optional<std::string> Store::put(std::string_view name, std::string_view text, std::string* error) {
  if (!valid_name(name)) return *error = "a spec's name is [a-z][a-z0-9_-]*, at most 64", std::nullopt;
  if (text.size() > (32u << 20)) return *error = "a spec is at most 32 MiB", std::nullopt;
  auto spec = parse(text, error);
  if (!spec) return std::nullopt;
  std::string sha = base::sha256_hex(text);
  fs::path file = dir_ / std::string(name) / (sha + ".json");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::string pin = std::string(name) + "@" + sha.substr(0, 12);
  std::lock_guard lock(mu_);
  // Another document already holding this pin's 48 bits keeps it: a pin never changes meaning.
  for (auto it = fs::directory_iterator(file.parent_path(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    std::string other = it->path().stem().string();
    if (other != sha && other.starts_with(sha.substr(0, 12))) {
      return *error = "another spec named " + std::string(name) + " already has pin " + pin + "; change a byte (a description) and put it again", std::nullopt;
    }
  }
  if (!fs::exists(file, ec) && !base::write_file_durable(file, text)) return *error = "couldn't write " + file.string(), std::nullopt;
  cache_[pin] = std::make_shared<const Spec>(std::move(*spec));
  return pin;
}

std::optional<fs::path> Store::file_of(std::string_view pin) {
  auto at = pin.find('@');
  if (at == std::string_view::npos || pin.size() != at + 13) return std::nullopt;
  std::string name(pin.substr(0, at)), prefix(pin.substr(at + 1));
  if (!valid_name(name)) return std::nullopt;
  for (char c : prefix) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  }
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir_ / name, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (it->path().filename().string().starts_with(prefix)) return it->path();
  }
  return std::nullopt;
}

std::shared_ptr<const Spec> Store::get(std::string_view pin, std::string* error) {
  {
    std::lock_guard lock(mu_);
    if (auto it = cache_.find(std::string(pin)); it != cache_.end()) return it->second;
  }
  auto file = file_of(pin);
  if (!file) return *error = "no spec " + std::string(pin) + " is stored (spec_put it)", nullptr;
  auto text = base::read_file(*file);
  if (!text) return *error = "couldn't read " + file->string(), nullptr;
  // The file is named by its whole hash: bytes that changed on disk aren't the pinned spec.
  if (base::sha256_hex(*text) != file->stem().string()) return *error = file->string() + " doesn't match its hash", nullptr;
  auto spec = parse(*text, error);
  if (!spec) return nullptr;
  auto shared = std::make_shared<const Spec>(std::move(*spec));
  std::lock_guard lock(mu_);
  cache_[std::string(pin)] = shared;
  return shared;
}

std::optional<std::string> Store::text(std::string_view pin) {
  auto file = file_of(pin);
  if (!file) return std::nullopt;
  return base::read_file(*file);
}

std::vector<Store::Listed> Store::list() {
  std::vector<Listed> out;
  std::error_code ec;
  for (auto d = fs::directory_iterator(dir_, ec); !ec && d != fs::directory_iterator(); d.increment(ec)) {
    if (!d->is_directory()) continue;
    std::error_code ec2;
    for (auto f = fs::directory_iterator(d->path(), ec2); !ec2 && f != fs::directory_iterator(); f.increment(ec2)) {
      std::string pin = d->path().filename().string() + "@" + f->path().filename().string().substr(0, 12);
      std::string err;
      if (auto s = get(pin, &err)) out.push_back(Listed{pin, s->format, s->title, s->version, s->ops.size()});
    }
  }
  std::sort(out.begin(), out.end(), [](const Listed& a, const Listed& b) { return a.pin < b.pin; });
  return out;
}

}  // namespace goober::specs

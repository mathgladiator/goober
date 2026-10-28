// Building source projects (design/machine/020.Source.md §4).

#include "builder.h"

#include <cstdlib>
#include <fstream>

#include "goober/base/files.h"
#include "goober/base/process.h"
#include "goober/base/time.h"
#include "rust_binding.h"

namespace goober::host {
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

constexpr size_t kLogTail = 32 << 10;

std::string tail(std::string text) {
  if (text.size() > kLogTail) text = "...\n" + text.substr(text.size() - kLogTail);
  return text;
}

std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : fallback;
}

// cargo: the config's, else the first on PATH, else rustup's usual place.
std::string find_cargo(const config::HostConfig& cfg) {
  if (!cfg.source.cargo.empty()) return cfg.source.cargo;
  std::string path = env_or("PATH", "/usr/bin:/bin");
  for (size_t a = 0, b; a <= path.size(); a = b + 1) {
    b = path.find(':', a);
    if (b == std::string::npos) b = path.size();
    fs::path candidate = fs::path(path.substr(a, b - a)) / "cargo";
    if (!candidate.parent_path().empty() && fs::exists(candidate)) return candidate.string();
  }
  fs::path home = fs::path(env_or("HOME", "/nonexistent")) / ".cargo/bin/cargo";
  return fs::exists(home) ? home.string() : "";
}

std::string find_rustup_home() {
  std::string home = env_or("RUSTUP_HOME", (fs::path(env_or("HOME", "/nonexistent")) / ".rustup").string());
  return fs::is_directory(home) ? home : "";
}

std::string find_program(const std::string& name) {
  for (const char* dir : {"/usr/bin", "/bin", "/usr/local/bin"}) {
    if (fs::exists(fs::path(dir) / name)) return (fs::path(dir) / name).string();
  }
  return "";
}

// The crate's name from Cargo.toml: [lib] name, else [package] name, with - as _.
std::string crate_name(const std::string& toml) {
  std::string section, package, lib;
  size_t start = 0;
  while (start < toml.size()) {
    size_t end = toml.find('\n', start);
    if (end == std::string::npos) end = toml.size();
    std::string line = toml.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t lead = line.find_first_not_of(" \t");
    if (lead == std::string::npos) continue;
    line = line.substr(lead);
    if (line.starts_with("[")) {
      section = line;
      continue;
    }
    if (!line.starts_with("name")) continue;
    auto q1 = line.find('"'), q2 = line.rfind('"');
    if (q1 == std::string::npos || q2 <= q1) continue;
    std::string value = line.substr(q1 + 1, q2 - q1 - 1);
    if (section == "[package]") package = value;
    if (section == "[lib]") lib = value;
  }
  std::string name = lib.empty() ? package : lib;
  for (auto& c : name) c = c == '-' ? '_' : c;
  return name;
}

struct ProjectSpec {
  std::vector<std::string> boxes, schemas;
  std::vector<std::pair<std::string, std::string>> sites;  // dir, tenant
};

std::optional<ProjectSpec> read_spec(const std::string& text, std::string* error) {
  std::string err;
  auto v = json::parse(text, &err);
  if (!v || !v->is_object()) return *error = "goober.project.json isn't a JSON object: " + err, std::nullopt;
  ProjectSpec spec;
  auto dir_ok = [](const std::string& d) { return d == "." || source::valid_path(d); };
  for (const auto& kv : v->as_object()) {
    std::string k(kv.key());
    if (k == "boxes" || k == "schemas") {
      if (!kv.value().is_array()) return *error = "goober.project.json: " + k + " is a list", std::nullopt;
      for (const auto& e : kv.value().as_array()) {
        std::string s = e.is_string() ? std::string(e.as_string()) : "";
        if (k == "boxes" ? !dir_ok(s) : !source::valid_path(s)) {
          return *error = "goober.project.json: " + k + " holds a bad path: " + s, std::nullopt;
        }
        (k == "boxes" ? spec.boxes : spec.schemas).push_back(s);
      }
    } else if (k == "sites") {
      if (!kv.value().is_array()) return *error = "goober.project.json: sites is a list", std::nullopt;
      for (const auto& e : kv.value().as_array()) {
        const auto* o = e.if_object();
        const auto* dir = o ? o->if_contains("dir") : nullptr;
        const auto* tenant = o ? o->if_contains("tenant") : nullptr;
        if (!dir || !dir->is_string() || !tenant || !tenant->is_string() || !dir_ok(std::string(dir->as_string()))) {
          return *error = "goober.project.json: a site is {\"dir\": \"site\", \"tenant\": \"playground\"}", std::nullopt;
        }
        spec.sites.emplace_back(std::string(dir->as_string()), std::string(tenant->as_string()));
      }
    } else if (k != "description") {
      return *error = "goober.project.json: unknown field \"" + k + "\" (boxes, schemas, sites)", std::nullopt;
    }
  }
  if (spec.boxes.empty() && spec.schemas.empty() && spec.sites.empty()) {
    return *error = "goober.project.json names no boxes, schemas, or sites", std::nullopt;
  }
  return spec;
}

std::string joined(const std::string& dir, const std::string& name) { return dir == "." ? name : dir + "/" + name; }

}  // namespace

std::vector<std::string> sandboxed(const std::vector<std::string>& command, const config::HostConfig& cfg,
                                   const fs::path& src, const fs::path& target, const std::string& cargo,
                                   const std::string& rustup_home, std::string* error) {
  std::string bwrap = find_program("bwrap");
  if (bwrap.empty()) {
    *error = "bubblewrap (bwrap) isn't installed, and builds run only in its sandbox: install it (apt install "
             "bubblewrap), or set source.sandbox to \"none\" on a machine you trust";
    return {};
  }
  std::string cargo_dir = fs::path(cargo).parent_path().string();
  std::vector<std::string> a{bwrap, "--unshare-all", "--die-with-parent", "--new-session", "--clearenv",
                             "--ro-bind", "/usr", "/usr"};
  for (const char* p : {"/lib", "/lib64", "/bin", "/sbin"}) {
    std::error_code ec;
    if (fs::is_symlink(p, ec)) {
      a.insert(a.end(), {"--symlink", fs::read_symlink(p, ec).string(), p});
    } else if (fs::is_directory(p, ec)) {
      a.insert(a.end(), {"--ro-bind", p, p});
    }
  }
  // Only what a compiler and a linker read from /etc: nothing like a password file.
  for (const char* p : {"/etc/alternatives", "/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d"}) {
    if (fs::exists(p)) a.insert(a.end(), {"--ro-bind", p, p});
  }
  a.insert(a.end(), {"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"});
  if (!cargo_dir.starts_with("/usr/")) a.insert(a.end(), {"--ro-bind", cargo_dir, cargo_dir});
  if (!rustup_home.empty()) a.insert(a.end(), {"--ro-bind", rustup_home, rustup_home, "--setenv", "RUSTUP_HOME", rustup_home});
  if (!cfg.source.vendor.empty()) a.insert(a.end(), {"--ro-bind", cfg.source.vendor, "/vendor"});
  a.insert(a.end(), {"--bind", src.string(), "/work", "--bind", target.string(), "/target", "--setenv", "HOME", "/work",
                     "--setenv", "CARGO_HOME", "/work/.cargo-home", "--setenv", "CARGO_TARGET_DIR", "/target",
                     "--setenv", "PATH", cargo_dir + ":/usr/bin:/bin", "--chdir", "/work"});
  a.insert(a.end(), command.begin(), command.end());
  return a;
}

std::optional<std::vector<source::Change>> template_files(const std::string& kind, const std::string& project,
                                                          const std::string& tenant, std::string* error) {
  // Route names are unique across a host's boxes, so the template's carry the box's name.
  std::string box = project;
  for (auto& c : box) c = c == '-' ? '_' : c;
  std::vector<source::Change> files;
  if (kind == "empty") return files;
  if (kind == "rust-box") {
    files.push_back({"goober.project.json", "{\n  \"boxes\": [\".\"]\n}\n"});
    files.push_back({"Cargo.toml", "[package]\nname = \"" + box +
                                       "\"\nversion = \"0.1.0\"\nedition = \"2024\"\npublish = false\n\n[lib]\n"
                                       "crate-type = [\"cdylib\"]\npath = \"src/lib.rs\"\n\n[dependencies]\n"
                                       "serde_json = \"1\"\n\n[profile.release]\nopt-level = \"s\"\nlto = true\n"
                                       "panic = \"abort\"\n"});
    files.push_back({"manifest.json", "{\n  \"box\": \"" + box + "\",\n  \"mission\": \"Say hello. Replace this with what the box is for.\",\n"
                                      "  \"domains\": [\"*\"],\n  \"routes\": {\n    \"http\": {\n      \"/" + box +
                                      "/hello\": { \"get\": { \"name\": \"" + box + "_hello\", \"public\": true } }\n    }\n  }\n}\n"});
    files.push_back({"src/lib.rs", "//! " + box + ": a box. See box_guide for the boundary, data, mail, and the web.\n\n"
                                   "mod goober;\n\nuse serde_json::json;\n\n#[unsafe(no_mangle)]\n"
                                   "pub extern \"C\" fn goober_route_" + box + "_hello() -> i32 {\n"
                                   "    goober::respond_json(200, &json!({ \"hello\": \"from " + box + "\", \"at_ms\": goober::now_ms() }));\n"
                                   "    0\n}\n"});
    files.push_back({"src/goober.rs", kRustBinding});
    return files;
  }
  if (kind == "wat-box") {
    files.push_back({"goober.project.json", "{\n  \"boxes\": [\".\"]\n}\n"});
    files.push_back({"manifest.json", "{\n  \"box\": \"" + box + "\",\n  \"mission\": \"Say hello.\",\n  \"domains\": [\"*\"],\n"
                                      "  \"routes\": { \"http\": { \"/" + box +
                                      "/hello\": { \"get\": { \"name\": \"" + box + "_hello\", \"public\": true } } } }\n}\n"});
    files.push_back({"box.wat", "(module\n  (import \"goober\" \"goober_respond\" (func $respond (param i32 i32 i32 i32 i32) (result i32)))\n"
                                "  (memory (export \"memory\") 1)\n  (data (i32.const 0) \"text/plain\")\n"
                                "  (data (i32.const 16) \"hello\")\n  (func (export \"goober_route_" + box + "_hello\") (result i32)\n"
                                "    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 16) (i32.const 5)))\n"
                                "    (i32.const 0)))\n"});
    return files;
  }
  if (kind == "site") {
    if (tenant.empty()) return *error = "a site template needs the tenant it's for", std::nullopt;
    files.push_back({"goober.project.json", "{\n  \"sites\": [{ \"dir\": \"site\", \"tenant\": \"" + tenant + "\" }]\n}\n"});
    files.push_back({"site/index.md", "---\ntitle: Home\n---\n# Hello\n\nThis page lives in the project " + project +
                                          ". Change it, commit, and deploy.\n"});
    return files;
  }
  return *error = "a template is empty, rust-box, wat-box, or site", std::nullopt;
}

BuildResult build_project(source::Store& store, const std::string& project, int64_t rev, const config::HostConfig& cfg,
                          const fs::path& work, const fs::path& cache) {
  BuildResult out;
  std::string err;
  auto fail = [&](const std::string& why) {
    out.log += "✗ " + why + "\n";
    out.ok = false;
    return out;
  };
  auto files = store.files(project, rev, &err);
  if (!files) return fail(err);
  auto read = [&](const std::string& path) { return store.read(project, rev, path, &err); };
  auto spec_text = read("goober.project.json");
  if (!spec_text) return fail("the project has no goober.project.json (say what it builds: boxes, schemas, sites)");
  auto spec = read_spec(*spec_text, &err);
  if (!spec) return fail(err);

  // The revision's files, written out for the compiler.
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::path src = work / "src";
  fs::create_directories(src);
  for (const auto& f : *files) {
    auto bytes = read(f.path);
    if (!bytes) return fail(err);
    fs::create_directories((src / f.path).parent_path());
    std::ofstream o(src / f.path, std::ios::binary);
    o.write(bytes->data(), std::streamsize(bytes->size()));
    if (!o) return fail("can't write " + f.path);
  }
  out.log += "revision " + std::to_string(rev) + " of " + project + ": " + std::to_string(files->size()) + " files\n";

  for (const auto& s : spec->schemas) {
    auto text = read(s);
    if (!text) return fail("schemas names " + s + ", which isn't in the project");
    out.schemas.emplace_back(s, *text);
  }
  for (const auto& dir : spec->boxes) {
    BuiltBox b;
    b.dir = dir;
    auto manifest = read(joined(dir, "manifest.json"));
    if (!manifest) return fail(joined(dir, "manifest.json") + " is missing");
    b.manifest = *manifest;
    std::string jerr;
    auto m = json::parse(b.manifest, &jerr);
    if (!m || !m->is_object() || !m->as_object().contains("box") || !m->at("box").is_string()) {
      return fail(joined(dir, "manifest.json") + " isn't a manifest with a box name: " + jerr);
    }
    b.box = std::string(m->at("box").as_string());
    if (auto* t = m->as_object().if_contains("types"); t && t->is_string()) {
      auto types = read(joined(dir, std::string(t->as_string())));
      if (!types) return fail(joined(dir, std::string(t->as_string())) + " (the manifest's types) is missing");
      b.types = *types;
    }
    if (auto wasm = read(joined(dir, "box.wasm"))) {
      b.module = *wasm;
      out.log += "box " + b.box + ": the committed box.wasm\n";
    } else if (auto wat = read(joined(dir, "box.wat"))) {
      b.module = *wat;
      out.log += "box " + b.box + ": box.wat\n";
    } else if (auto toml = read(joined(dir, "Cargo.toml"))) {
      std::string crate = crate_name(*toml);
      if (crate.empty()) return fail(joined(dir, "Cargo.toml") + " has no package name");
      std::string cargo = find_cargo(cfg);
      if (cargo.empty()) {
        return fail("this host has no Rust toolchain (cargo with the wasm32-unknown-unknown target); see "
                    "tools/server-bootstrap.sh --with-rust, or commit a built box.wasm");
      }
      std::string rustup_home = find_rustup_home();
      fs::path target = cache / "target";
      fs::create_directories(target);
      bool sandbox = cfg.source.sandbox != "none";
      // Offline, from the host's vendored crates when it has them.
      std::string vendor = sandbox ? "/vendor" : cfg.source.vendor;
      std::string cargo_config = "[net]\noffline = true\n";
      if (!cfg.source.vendor.empty()) {
        cargo_config += "\n[source.crates-io]\nreplace-with = \"vendored\"\n\n[source.vendored]\ndirectory = \"" + vendor + "\"\n";
      }
      if (!base::write_file_durable(src / ".cargo-home" / "config.toml", cargo_config)) return fail("can't write cargo's config");
      std::vector<std::string> command{sandbox ? "cargo" : cargo, "build", "--release", "--target", "wasm32-unknown-unknown",
                                       "--offline", "--manifest-path",
                                       (sandbox ? fs::path("/work") : src).string() + "/" + joined(dir, "Cargo.toml")};
      base::ProcessOptions po;
      po.max_err = 1 << 20;
      po.own_group = true;
      std::vector<std::string> argv;
      if (sandbox) {
        argv = sandboxed(command, cfg, src, target, cargo, rustup_home, &err);
        if (argv.empty()) return fail(err);
        po.env = std::vector<std::string>{};
      } else {
        argv = command;
        po.cwd = src;
        po.env = std::vector<std::string>{"HOME=" + src.string(), "CARGO_HOME=" + (src / ".cargo-home").string(),
                                          "CARGO_TARGET_DIR=" + target.string(),
                                          "PATH=" + fs::path(cargo).parent_path().string() + ":/usr/bin:/bin"};
        if (!rustup_home.empty()) po.env->push_back("RUSTUP_HOME=" + rustup_home);
      }
      out.log += "box " + b.box + ": cargo build --release --target wasm32-unknown-unknown" +
                 (sandbox ? " (in the sandbox: no network)" : " (no sandbox)") + "\n";
      int64_t started = base::monotonic_us();
      auto r = base::run_process(argv, "", 1 << 20, cfg.source.build_timeout_ms, po);
      out.log += tail(r.out + r.err);
      out.log += "(" + std::to_string((base::monotonic_us() - started) / 1000) + " ms)\n";
      if (!r.started) return fail("couldn't run the build");
      if (r.timed_out) return fail("the build took longer than " + std::to_string(cfg.source.build_timeout_ms / 1000) + " s");
      if (r.exit_code != 0) return fail("cargo failed (exit " + std::to_string(r.exit_code) + ")");
      auto wasm = base::read_file(target / "wasm32-unknown-unknown" / "release" / (crate + ".wasm"));
      if (!wasm) return fail("cargo succeeded but " + crate + ".wasm isn't there: is the crate a cdylib?");
      b.module = std::move(*wasm);
    } else {
      return fail("box " + dir + " has no Cargo.toml, box.wat, or box.wasm");
    }
    out.boxes.push_back(std::move(b));
  }
  for (const auto& [dir, tenant] : spec->sites) {
    BuiltSite site{dir, tenant, {}};
    std::string prefix = dir == "." ? "" : dir + "/";
    for (const auto& f : *files) {
      if (!f.path.starts_with(prefix) || f.path == "goober.project.json") continue;
      auto bytes = read(f.path);
      if (!bytes) return fail(err);
      site.files.emplace_back(f.path.substr(prefix.size()), std::move(*bytes));
    }
    if (site.files.empty()) return fail("site " + dir + " holds no files");
    out.log += "site " + dir + " for " + tenant + ": " + std::to_string(site.files.size()) + " files\n";
    out.sites.push_back(std::move(site));
  }
  fs::remove_all(work, ec);
  out.ok = true;
  out.log += "✓ built\n";
  return out;
}

BuildJobs::BuildJobs() : worker_([this] { run(); }) {}

BuildJobs::~BuildJobs() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

int64_t BuildJobs::submit(Job job, Work work) {
  std::lock_guard lock(mu_);
  job.id = next_id_++;
  job.queued_ms = base::now_unix_ms();
  jobs_[job.id] = job;
  queue_.emplace_back(job.id, std::move(work));
  // Only the newest hundred jobs are remembered.
  while (jobs_.size() > 100 && jobs_.begin()->second.state != "queued" && jobs_.begin()->second.state != "running") {
    jobs_.erase(jobs_.begin());
  }
  cv_.notify_all();
  return job.id;
}

std::optional<BuildJob> BuildJobs::get(int64_t id, int64_t wait_ms) {
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
    auto it = jobs_.find(id);
    return it == jobs_.end() || it->second.state == "succeeded" || it->second.state == "failed";
  });
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::nullopt;
  return it->second;
}

std::vector<BuildJob> BuildJobs::recent() {
  std::lock_guard lock(mu_);
  std::vector<Job> out;
  for (auto it = jobs_.rbegin(); it != jobs_.rend() && out.size() < 20; ++it) out.push_back(it->second);
  return out;
}

void BuildJobs::run() {
  for (;;) {
    std::pair<int64_t, Work> next;
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      next = std::move(queue_.front());
      queue_.pop_front();
      auto& j = jobs_[next.first];
      j.state = "running";
      j.started_ms = base::now_unix_ms();
      job = j;
    }
    try {
      next.second(job);
    } catch (const std::exception& e) {
      job.state = "failed";
      job.log += std::string("✗ ") + e.what() + "\n";
    }
    if (job.state == "running") job.state = "failed";
    job.finished_ms = base::now_unix_ms();
    {
      std::lock_guard lock(mu_);
      jobs_[job.id] = job;
    }
    cv_.notify_all();
  }
}

}  // namespace goober::host

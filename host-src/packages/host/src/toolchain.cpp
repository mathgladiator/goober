// goober-host toolchain: proves this host can build a Rust box the way source_deploy does
// (design/machine/020.Source.md § 6). It builds the rust-box template, with a derive so a
// proc macro and the crates' build scripts run, through build_project: the configured cargo,
// the wasm32 target, a C linker for build scripts, the vendored crates, offline, in the sandbox.
// Deploys run it last, so a missing piece fails the deploy instead of an agent's first build.

#include <iostream>

#include "builder.h"
#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/config/config.h"
#include "goober/source/source.h"

namespace goober::host {

namespace fs = std::filesystem;

int run_toolchain_check(const std::string& config_path) {
  std::string err;
  auto cfg = config::load_config(config_path, &err);
  if (!cfg) return std::cerr << "goober-host: " << err << "\n", 1;
  uint8_t rnd[6];
  base::os_random_fill(rnd);
  fs::path dir = cfg->storage / "tmp" / ("toolchain-" + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), 6)));
  fs::create_directories(dir);
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{dir};

  const std::string name = "toolchain-check";
  int64_t now = base::now_unix_ms();
  source::Store store(dir / "source.db");
  if (!store.create(name, "goober-host toolchain", now, &err)) return std::cerr << "goober-host: " << err << "\n", 1;
  auto files = template_files("rust-box", name, "", &err);
  if (!files) return std::cerr << "goober-host: " << err << "\n", 1;
  for (auto& f : *files) {
    // serde's derive: a proc macro, built for this machine with its C linker, like most crates.
    if (f.path == "Cargo.toml") {
      auto at = f.content->find("serde_json = \"1\"\n");
      f.content->insert(at, "serde = { version = \"1\", features = [\"derive\"] }\n");
    }
    if (f.path == "src/lib.rs") {
      *f.content += "\n#[derive(serde::Serialize)]\nstruct Check {\n    ok: bool,\n}\n\n"
                    "pub fn toolchain_check() -> String {\n    serde_json::to_string(&Check { ok: true }).unwrap_or_default()\n}\n";
    }
  }
  auto rev = store.commit(name, 0, *files, "toolchain check", "goober-host", now, &err);
  if (!rev) return std::cerr << "goober-host: " << err << "\n", 1;
  std::cout << "building a Rust box (serde with derive), offline, with the vendored crates...\n"
            << std::flush;
  auto result = build_project(store, name, rev->rev, *cfg, dir / "work", dir / "cache");
  if (!result.ok || result.boxes.empty()) {
    std::cerr << result.log << "\n✗ this host can't build Rust boxes; the build's log is above\n";
    return 1;
  }
  std::cout << "✓ toolchain: cargo, the wasm32 target, a C linker, and the vendored crates work"
            << (cfg->source.sandbox == "bwrap" ? ", in the sandbox (bwrap)" : ", without a sandbox (source.sandbox none)")
            << " (" << result.boxes[0].module.size() << "-byte box)\n";
  // A production host builds agents' code in the sandbox; saying so keeps "none" a choice.
  if (cfg->source.sandbox != "bwrap" && !cfg->local) {
    std::cerr << "✗ source.sandbox is \"none\" on a host that isn't local: agents' builds run unsandboxed\n";
    return 1;
  }
  return 0;
}

}  // namespace goober::host

// goober-host: serve static content and WASM boxes from one JSON config.
//
//   goober-host [--config host.json] [--local]     run the host
//   goober-host check [--config host.json]         validate the config and exit
//   goober-host token create [--config host.json] [--name NAME] [--claude] [--local]
//   goober-host token list [--config host.json]
//   goober-host token revoke NAME_OR_ID [--config host.json]
//   goober-host db list [TENANT] [--config host.json]
//   goober-host db describe TENANT DATABASE [--config host.json]
//   goober-host db query TENANT DATABASE SQL [--config host.json]
//   goober-host db exec TENANT DATABASE SQL [--config host.json]
//   goober-host backup DIR [--config host.json]      a consistent copy, while running
//   goober-host restore DIR [--config host.json]     into an empty storage, host stopped
//
// The db commands read the database files directly, so they work whether or not the
// host is running; exec is logged to logs/db.log as the operator "cli".
//
// --local is for playing on one machine: listen on 127.0.0.1, plain session
// cookies, sign-in codes printed here, and "$domain.localhost" reaching $domain.
// SIGHUP rereads the config; SIGINT and SIGTERM stop the host.

#include <sys/stat.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#include "goober/base/files.h"
#include "goober/json/json.h"

#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"

namespace {

constexpr const char* kUsage =
    "usage: goober-host [--config host.json] [--local]\n"
    "       goober-host check [--config host.json]\n"
    "       goober-host token create [--config host.json] [--name NAME] [--claude] [--local]\n"
    "       goober-host token list [--config host.json]\n"
    "       goober-host token revoke NAME_OR_ID [--config host.json]\n"
    "       goober-host db list [TENANT] [--config host.json]\n"
    "       goober-host db describe TENANT DATABASE [--config host.json]\n"
    "       goober-host db query TENANT DATABASE SQL [--config host.json]\n"
    "       goober-host db exec TENANT DATABASE SQL [--config host.json]\n"
    "       goober-host backup DIR [--config host.json]\n"
    "       goober-host restore DIR [--config host.json]\n";

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

int run_token(const std::vector<std::string>& args, const std::string& config_path, const std::string& name,
              bool claude, bool local) {
  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) {
    std::cerr << "goober-host: " << err << "\n";
    return 1;
  }
  if (local) config = goober::config::local_mode(config);
  const std::string& verb = args.size() > 1 ? args[1] : "";
  if (verb == "create") {
    auto token = goober::host::create_operator_token(config->storage, name, goober::base::now_unix_ms(), &err);
    if (!token) {
      std::cerr << "goober-host: " << err << "\n";
      return 1;
    }
    if (!claude) {
      std::cout << *token << "\n";
      return 0;
    }
    std::string url = config->base_url() + std::string(goober::host::kOperationsMcpPath);
    std::cout << "claude mcp add --transport http goober " << shell_quote(url) << " --header "
              << shell_quote("Authorization: Bearer " + *token) << "\n";
    return 0;
  }
  if (verb == "list") {
    for (const auto& t : goober::host::list_operator_tokens(config->storage)) {
      std::cout << t.id << "  " << goober::base::format_utc_ms(t.created_ms) << "  " << t.name << "\n";
    }
    return 0;
  }
  if (verb == "revoke" && args.size() > 2) {
    int n = goober::host::revoke_operator_tokens(config->storage, args[2]);
    std::cout << "revoked " << n << " token" << (n == 1 ? "" : "s") << "\n";
    return n > 0 ? 0 : 1;
  }
  std::cerr << kUsage;
  return 2;
}

// A value as one cell of a text table.
std::string cell(const boost::json::value& v) {
  if (v.is_string()) return std::string(v.get_string());
  if (v.is_null()) return "NULL";
  return goober::json::dump(v);
}

int run_db(const std::vector<std::string>& args, const std::string& config_path) {
  namespace bj = boost::json;
  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) {
    std::cerr << "goober-host: " << err << "\n";
    return 1;
  }
  goober::db::Databases dbs(config->storage);
  const std::string& verb = args.size() > 1 ? args[1] : "list";
  if (verb == "list") {
    std::set<std::string> tenants;
    for (const auto& [d, e] : config->domains) tenants.insert(e.tenant);
    for (const auto& t : dbs.tenants()) tenants.insert(t);
    if (args.size() > 2) tenants = {args[2]};
    for (const auto& tenant : tenants) {
      auto names = dbs.databases(tenant);
      std::cout << tenant << (names.empty() ? "  (no databases yet)" : "") << "\n";
      for (const auto& name : names) {
        auto d = dbs.describe(tenant, name);
        std::cout << "  " << name << "  " << d.at("bytes").as_int64() / 1024 << " KiB\n";
        for (const auto& t : d.at("tables").as_array()) {
          std::cout << "    " << cell(t.at("table")) << "  " << cell(t.at("rows")) << " rows\n";
        }
      }
    }
    return 0;
  }
  if (verb == "describe" && args.size() == 4) {
    std::cout << goober::json::pretty(dbs.describe(args[2], args[3])) << "\n";
    return 0;
  }
  if ((verb == "query" || verb == "exec") && args.size() == 5) {
    bool write = verb == "exec";
    auto r = dbs.operate(args[2], args[3], args[4], nullptr, write ? 1000 : 10000, write, &err);
    if (write) {
      goober::base::append_line(config->storage / "logs" / "db.log",
                                goober::json::dump(bj::object{{"at", goober::base::format_utc_ms(goober::base::now_unix_ms())},
                                                              {"operator", "cli"},
                                                              {"tenant", args[2]},
                                                              {"database", args[3]},
                                                              {"sql", args[4]},
                                                              {"changes", r ? r->at("changes") : bj::value(nullptr)},
                                                              {"error", r ? "" : err}}));
    }
    if (!r) {
      std::cerr << "goober-host: " << err << "\n";
      return 1;
    }
    if (write) {
      std::cout << "changed " << cell(r->at("changes")) << " row" << (r->at("changes") == 1 ? "" : "s") << "\n";
      return 0;
    }
    const auto& cols = r->at("columns").as_array();
    const auto& rows = r->at("rows").as_array();
    std::vector<size_t> width(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) width[i] = cell(cols[i]).size();
    for (const auto& row : rows) {
      for (size_t i = 0; i < cols.size(); ++i) width[i] = std::max(width[i], std::min<size_t>(60, cell(row.at(i)).size()));
    }
    auto line = [&](const bj::array& values) {
      for (size_t i = 0; i < values.size(); ++i) {
        std::string c = cell(values[i]);
        if (c.size() > 60) c = c.substr(0, 57) + "...";
        std::cout << (i ? "  " : "") << c << std::string(width[i] - std::min(width[i], c.size()), ' ');
      }
      std::cout << "\n";
    };
    line(cols);
    for (size_t i = 0; i < cols.size(); ++i) std::cout << (i ? "  " : "") << std::string(width[i], '-');
    std::cout << "\n";
    for (const auto& row : rows) line(row.as_array());
    std::cout << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")\n";
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  // Everything the host writes (identity databases, tokens, the WAL, content, logs) is
  // readable by its own user only.
  umask(0077);
  std::string config_path = "host.json";
  std::string token_name = "operator";
  bool local = false, claude = false;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--name" && i + 1 < argc) {
      token_name = argv[++i];
    } else if (a == "--local") {
      local = true;
    } else if (a == "--claude") {
      claude = true;
    } else if (a == "--version") {
      std::cout << "goober-host " << goober::host::kVersion << "\n";
      return 0;
    } else if (!a.starts_with("-")) {
      positional.push_back(a);
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (!positional.empty() && positional[0] == "token") return run_token(positional, config_path, token_name, claude, local);
  if (!positional.empty() && positional[0] == "db") return run_db(positional, config_path);
  if (!positional.empty() && (positional[0] == "backup" || positional[0] == "restore")) {
    if (positional.size() != 2) return std::cerr << kUsage, 2;
    std::string err;
    auto config = goober::config::load_config(config_path, &err);
    if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
    goober::host::BackupReport report;
    bool ok = positional[0] == "backup"
                  ? goober::host::backup_storage(config->storage, positional[1], &report, &err)
                  : goober::host::restore_storage(positional[1], config->storage, &report, &err);
    if (!ok) return std::cerr << "goober-host: " << err << "\n", 1;
    std::cout << (positional[0] == "backup" ? "backed up " : "restored ") << report.databases << " databases and "
              << report.files << " files (" << report.bytes / 1024 << " KiB)\n";
    return 0;
  }
  if (!positional.empty() && positional[0] != "check") {
    std::cerr << kUsage;
    return 2;
  }

  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) {
    std::cerr << "goober-host: " << err << "\n";
    return 1;
  }
  if (!positional.empty()) {
    std::cout << config_path << ": ok\n";
    return 0;
  }
  if (local) config = goober::config::local_mode(config);

  // Block the signals before any thread starts, so every thread inherits the
  // mask and only sigwait below receives them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGHUP);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  try {
    goober::host::Host host(config, config_path);
    for (const auto& note : host.startup_notes()) std::cerr << "goober-host: " << note << "\n";
    uint16_t port = host.start();
    std::string base = "http://" + config->listen_address + ":" + std::to_string(port);
    std::cerr << "goober-host " << goober::host::kVersion << " listening on " << base
              << (local ? " (local mode)" : "") << "\n"
              << "  storage:    " << config->storage.string() << "\n"
              << "  operations: " << base << goober::host::kOperationsMcpPath << " (MCP, bearer token)\n"
              << "  sign in:    /identity/sign-in on any domain (operators: /identity/sign-in/as-host); "
                 "account at /identity/account\n";
    for (const auto& [domain, entry] : config->domains) {
      std::cerr << "  domain:     " << domain << " -> tenant " << entry.tenant;
      if (local && domain != "localhost" && domain != "127.0.0.1" && !domain.ends_with(".localhost")) {
        std::cerr << "  (http://" << domain << ".localhost:" << port << "/)";
      }
      std::cerr << "\n";
    }
    while (true) {
      int sig = 0;
      sigwait(&signals, &sig);
      if (sig == SIGHUP) {
        std::cerr << "goober-host: " << host.reload() << "\n";
        continue;
      }
      std::cerr << "goober-host: stopping\n";
      host.stop();
      break;
    }
  } catch (const std::exception& e) {
    std::cerr << "goober-host: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

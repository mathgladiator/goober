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
//   goober-host key set NAME [--hosts a.com,*.b.com] [--tenant T] [--config host.json]   the value on stdin
//   goober-host key list [--config host.json]
//   goober-host backup key [--confirm] [--config host.json]      print the S3 backups' key
//   goober-host backup list [--config host.json]                 restore points in the bucket
//   goober-host toolchain [--config host.json]
//   goober-host rehydrate DIR [--at MOMENT] [--from host.json | --bucket B [--region R]
//                [--endpoint URL] [--prefix P]] [--key-file F] [--keep-keys] [--port N]
//       a sandboxed devbox from S3; AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY (and
//       AWS_SESSION_TOKEN), and GOOBER_BACKUP_KEY unless --key-file
//
// The db commands read the database files directly, so they work whether or not the
// host is running; exec is logged to logs/db.log as the operator "cli".
//
// --local is for playing on one machine: listen on 127.0.0.1, plain session
// cookies, sign-in codes printed here, and "$domain.localhost" reaching $domain.
// SIGHUP rereads the config; SIGINT and SIGTERM stop the host.

#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#include "goober/base/files.h"
#include "goober/json/json.h"

#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "build_id.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "../src/backups.h"
#include "../src/keys.h"

namespace goober::host {
// goober-host toolchain (toolchain.cpp): builds a Rust box the way source_deploy does.
int run_toolchain_check(const std::string& config_path);
}  // namespace goober::host

namespace {

constexpr const char* kUsage =
    "usage: goober-host [--config host.json] [--local]\n"
    "       goober-host check [--config host.json]\n"
    "       goober-host token create [--config host.json] [--name NAME] [--tenants T,...] [--claude] [--local]\n"
    "       goober-host token list [--config host.json]\n"
    "       goober-host token revoke NAME_OR_ID [--config host.json]\n"
    "       goober-host operator revoke EMAIL [--config host.json]   (ends every operator login and OAuth grant)\n"
    "       goober-host db list [TENANT] [--config host.json]\n"
    "       goober-host db describe TENANT DATABASE [--config host.json]\n"
    "       goober-host db query TENANT DATABASE SQL [--config host.json]\n"
    "       goober-host db exec TENANT DATABASE SQL [--config host.json]\n"
    "       goober-host backup DIR [--config host.json]\n"
    "       goober-host restore DIR [--config host.json]\n"
    "       goober-host key set NAME [--hosts H,...] [--tenant T] [--config host.json]   (the value on stdin)\n"
    "       goober-host key list [--config host.json]\n"
    "       goober-host backup key [--confirm] [--config host.json]\n"
    "       goober-host vault key [--config host.json]   (prints the key that seals people's secrets)\n"
    "       goober-host backup list [--config host.json]\n"
    "       goober-host toolchain [--config host.json]   (builds a Rust box as source_deploy would)\n"
    "       goober-host rehydrate DIR [--at MOMENT] [--from host.json | --bucket B [--region R] [--endpoint URL]\n"
    "                  [--prefix P]] [--key-file F] [--keep-keys] [--port N]\n";

namespace bj = boost::json;

// The CLI's own audit and db lines, through the same chained logs the host writes (032): the
// writer takes the file's lock and re-reads its tail, so the chain stays one chain.
void cli_log(const goober::config::HostConfig& config, const std::string& log, bj::object line) {
  std::string host = config.public_url;
  if (auto at = host.find("://"); at != std::string::npos) host = host.substr(at + 3);
  host = host.substr(0, host.find_first_of(":/"));
  if (host.empty()) host = config.platform_domain;
  goober::logs::Set set(config.storage / "logs", host);
  set.write(log, line, goober::base::now_unix_ms());
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

int run_token(const std::vector<std::string>& args, const std::string& config_path, const std::string& name,
              bool claude, bool local, const std::string& tenants) {
  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) {
    std::cerr << "goober-host: " << err << "\n";
    return 1;
  }
  if (local) config = goober::config::local_mode(config);
  const std::string& verb = args.size() > 1 ? args[1] : "";
  if (verb == "create") {
    // --tenants a,b: the token starts focused on those tenants (030); a person widens it in the panel.
    std::set<std::string> focus;
    for (size_t i = 0; i < tenants.size();) {
      size_t j = tenants.find(',', i);
      if (j == std::string::npos) j = tenants.size();
      if (j > i) focus.insert(tenants.substr(i, j - i));
      i = j + 1;
    }
    for (const auto& t : focus) {
      bool known = false;
      for (const auto& [d, e] : config->domains) known |= e.tenant == t;
      if (!known) {
        std::cerr << "goober-host: no domain in the host config serves tenant " << t << "\n";
        return 1;
      }
    }
    auto token = goober::host::create_operator_token(config->storage, name, goober::base::now_unix_ms(), &err);
    if (!token) {
      std::cerr << "goober-host: " << err << "\n";
      return 1;
    }
    cli_log(*config, "audit", bj::object{{"action", "token.create"}, {"actor", "cli"}, {"target", "token:" + name}, {"outcome", "ok"},
                                         {"tenants", tenants}});
    if (!focus.empty() && !goober::host::write_operator_scope(config->storage, "token:" + name, focus, "cli",
                                                             goober::base::now_unix_ms(), &err)) {
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
    cli_log(*config, "audit", bj::object{{"action", "token.revoke"}, {"actor", "cli"}, {"target", args[2]},
                                         {"outcome", n > 0 ? "ok" : "refused"}, {"count", n}});
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
      cli_log(*config, "db", (bj::object{
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

struct Flags {
  std::string tenant, at, from, bucket, region, endpoint, prefix, key_file, hosts;
  bool confirm = false, keep_keys = false;
  uint16_t port = 8080;
};

std::string read_stdin() {
  std::string value, line;
  while (std::getline(std::cin, line)) value += line + "\n";
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

int run_key(const std::vector<std::string>& args, const std::string& config_path, const Flags& flags) {
  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
  goober::host::KeyStore keys(config->storage);
  const std::string& verb = args.size() > 1 ? args[1] : "";
  if (verb == "set" && args.size() == 3) {
    std::vector<std::string> hosts;
    for (size_t a = 0, b; !flags.hosts.empty() && a <= flags.hosts.size(); a = b + 1) {
      b = flags.hosts.find(',', a);
      if (b == std::string::npos) b = flags.hosts.size();
      if (b > a) hosts.push_back(flags.hosts.substr(a, b - a));
    }
    // A box's key goes only where its hosts say; the host's own keys go to no box at all.
    bool own = flags.tenant.empty() && (args[2].starts_with("aws_") || args[2] == "backup_key");
    if (hosts.empty() && !own) return std::cerr << "goober-host: --hosts is required: where the key may be sent\n", 2;
    // From stdin, so a secret is never on a command line or in shell history.
    if (isatty(0)) std::cerr << "value for " << args[2] << " (then Enter): ";
    std::string value = read_stdin();
    if (!keys.set(flags.tenant.empty() ? "system" : "tenant", flags.tenant, args[2], value,
                  goober::base::now_unix_ms(), &err, hosts)) {
      return std::cerr << "goober-host: " << err << "\n", 1;
    }
    std::cout << "set keys/" << (flags.tenant.empty() ? "system" : "tenant/" + flags.tenant) << "/" << args[2] << "\n";
    return 0;
  }
  if (verb == "list") {
    for (const auto& k : keys.list()) {
      std::cout << k.at("key").as_string() << "  " << k.at("set_at").as_string() << "  " << k.at("fingerprint").as_string()
                << "\n";
    }
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}

int run_backup_cli(const std::vector<std::string>& args, const std::string& config_path, const Flags& flags) {
  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
  goober::host::KeyStore keys(config->storage);
  goober::host::BackupService service([config] { return config; }, keys, config_path);
  if (args[1] == "key") {
    auto key = service.key(&err);
    if (!key) return std::cerr << "goober-host: " << err << "\n", 1;
    if (flags.confirm) {
      service.confirm_key();
      std::cout << "confirmed: a copy of the backup key is kept somewhere else\n";
      return 0;
    }
    std::cout << goober::backup::key_hex(*key) << "\n";
    std::cerr << "Keep this key somewhere other than this machine (a password manager). Without it no backup can be\n"
                 "read. Then run: goober-host backup key --confirm\n";
    return 0;
  }
  if (args[1] == "list") {
    if (!config->backup.enabled()) return std::cerr << "goober-host: backups are off in " << config_path << "\n", 1;
    goober::aws::S3 s3({config->backup.bucket, config->backup.region.empty() ? config->aws.region : config->backup.region,
                        config->backup.endpoint, config->backup.prefix},
                       [&keys] { return keys.aws_credentials(); }, config->storage / "tmp");
    auto points = goober::host::list_restore_points(s3, &err);
    if (!points) return std::cerr << "goober-host: " << err << "\n", 1;
    for (const auto& p : *points) {
      std::cout << goober::base::format_utc_ms(p.at_ms) << "  " << (p.increment.empty() ? "nightly" : "increment") << "\n";
    }
    std::cout << "(" << points->size() << " restore points)\n";
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}

int run_rehydrate(const std::vector<std::string>& args, const Flags& flags) {
  if (args.size() != 2) return std::cerr << kUsage, 2;
  std::string err;
  goober::host::RehydrateOptions o;
  o.dir = args[1];
  o.port = flags.port;
  o.keep_keys = flags.keep_keys;
  if (!flags.from.empty()) {
    auto from = goober::config::load_config(flags.from, &err);
    if (!from) return std::cerr << "goober-host: " << err << "\n", 1;
    if (!from->backup.enabled()) return std::cerr << "goober-host: " << flags.from << " has no backup.s3\n", 1;
    o.location = {from->backup.bucket, from->backup.region.empty() ? from->aws.region : from->backup.region,
                  from->backup.endpoint, from->backup.prefix};
  }
  if (!flags.bucket.empty()) o.location.bucket = flags.bucket;
  if (!flags.region.empty()) o.location.region = flags.region;
  if (!flags.endpoint.empty()) o.location.endpoint = flags.endpoint;
  if (!flags.prefix.empty()) o.location.prefix = flags.prefix.back() == '/' ? flags.prefix : flags.prefix + "/";
  if (o.location.bucket.empty()) return std::cerr << "goober-host: say where: --from host.json or --bucket\n", 2;
  if (o.location.region.empty()) o.location.region = "us-east-1";
  auto env = [](const char* k) { return std::string(std::getenv(k) ? std::getenv(k) : ""); };
  o.credentials = {env("AWS_ACCESS_KEY_ID"), env("AWS_SECRET_ACCESS_KEY"), env("AWS_SESSION_TOKEN")};
  if (o.credentials.access_key_id.empty() || o.credentials.secret_access_key.empty()) {
    return std::cerr << "goober-host: set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY\n", 2;
  }
  std::string key_text = flags.key_file.empty() ? env("GOOBER_BACKUP_KEY") : goober::base::read_file(flags.key_file).value_or("");
  auto key = goober::backup::parse_key(key_text);
  if (!key) return std::cerr << "goober-host: the backup key (GOOBER_BACKUP_KEY or --key-file) must be 64 hex digits\n", 2;
  o.key = *key;
  if (!flags.at.empty()) {
    auto at = goober::backup::parse_moment(flags.at);
    if (!at) return std::cerr << "goober-host: --at is like 2026-10-26, 2026-10-26T18:00, or 2026-10-26T18:00:00Z (UTC)\n", 2;
    o.at_ms = *at;
  }
  goober::host::RehydrateReport report;
  if (!goober::host::rehydrate(o, &report, &err)) return std::cerr << "goober-host: " << err << "\n", 1;
  std::cout << "rehydrated " << o.dir.string() << " from the night of " << report.night
            << (report.increment.empty() ? "" : " and the increment at " + report.increment) << ":\n"
            << "  " << report.databases << " databases (" << report.deltas << " from increments), " << report.assets
            << " attachments" << (report.missing_assets ? " (" + std::to_string(report.missing_assets) + " missing)" : "")
            << "\n";
  for (const auto& n : report.notes) std::cout << "  note: " << n << "\n";
  std::cout << "It is a sandbox: no mail, calls, deliveries, or backups leave it. Run it with:\n"
            << "  goober-host --config " << (o.dir / "host.json").string() << " --local\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Everything the host writes (identity databases, tokens, the WAL, content, logs) is
  // readable by its own user only.
  umask(0077);
  std::string config_path = "host.json";
  std::string token_name = "operator";
  std::string token_tenants;
  bool local = false, claude = false;
  Flags flags;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--name" && i + 1 < argc) {
      token_name = argv[++i];
    } else if (a == "--tenants" && i + 1 < argc) {
      token_tenants = argv[++i];
    } else if (a == "--local") {
      local = true;
    } else if (a == "--claude") {
      claude = true;
    } else if (a == "--confirm") {
      flags.confirm = true;
    } else if (a == "--keep-keys") {
      flags.keep_keys = true;
    } else if ((a == "--hosts" || a == "--tenant" || a == "--at" || a == "--from" || a == "--bucket" || a == "--region" ||
                a == "--endpoint" || a == "--prefix" || a == "--key-file" || a == "--port") && i + 1 < argc) {
      std::string v = argv[++i];
      if (a == "--tenant") flags.tenant = v;
      if (a == "--hosts") flags.hosts = v;
      if (a == "--at") flags.at = v;
      if (a == "--from") flags.from = v;
      if (a == "--bucket") flags.bucket = v;
      if (a == "--region") flags.region = v;
      if (a == "--endpoint") flags.endpoint = v;
      if (a == "--prefix") flags.prefix = v;
      if (a == "--key-file") flags.key_file = v;
      if (a == "--port") flags.port = uint16_t(std::atoi(v.c_str()));
    } else if (a == "--version") {
      std::cout << "goober-host " << goober::host::kVersion << " (" << kBuildCommit << ")\n";
      return 0;
    } else if (!a.starts_with("-")) {
      positional.push_back(a);
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (!positional.empty() && positional[0] == "token") return run_token(positional, config_path, token_name, claude, local, token_tenants);
  if (!positional.empty() && positional[0] == "db") return run_db(positional, config_path);
  if (!positional.empty() && positional[0] == "key") return run_key(positional, config_path, flags);
  if (!positional.empty() && positional[0] == "rehydrate") return run_rehydrate(positional, flags);
  if (positional.size() == 1 && positional[0] == "toolchain") return goober::host::run_toolchain_check(config_path);
  if (positional.size() == 3 && positional[0] == "operator" && positional[1] == "revoke") {
    // Every login and token a host operator holds, ended: browser sessions and the OAuth grants
    // agents got through /oauth/operator (Grok's). Minted tokens are `token revoke`.
    std::string err;
    auto config = goober::config::load_config(config_path, &err);
    if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
    goober::identity::Identity id(config->storage / "identity", [](const goober::identity::Mail&) {});
    int n = id.revoke_sessions(goober::identity::kHostRealm, positional[2], "cli", goober::base::now_unix_ms());
    cli_log(*config, "audit", bj::object{{"action", "operator.revoke"}, {"actor", "cli"}, {"outcome", "ok"}, {"count", n},
                                         {"target", "operator:" + positional[2]}});
    std::cout << "ended " << n << " session(s) and token(s) for " << positional[2]
              << "; minted operator tokens are listed by `goober-host token list`\n";
    return 0;
  }
  // The vault key (034 § 4): it seals every person's secrets the boxes keep. Printed for safekeeping.
  if (positional.size() == 2 && positional[0] == "vault" && positional[1] == "key") {
    std::string err;
    auto config = goober::config::load_config(config_path, &err);
    if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
    goober::host::KeyStore keys(config->storage);
    auto v = keys.resolve("keys/system/vault_key", "");
    if (!v) {
      std::cout << "no vault key yet: the host makes one the first time a person connects an account\n";
      return 0;
    }
    std::cout << *v << "\n";
    std::cerr << "Keep this key somewhere other than this machine, beside the backup key. Without it every sealed\n"
                 "secret (people's API keys and connected accounts) is lost, and people must connect again.\n";
    return 0;
  }
  if (positional.size() == 2 && positional[0] == "backup" && (positional[1] == "key" || positional[1] == "list")) {
    return run_backup_cli(positional, config_path, flags);
  }
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
              << (config->panel_domain().empty() ? std::string()
                                                 : "  panel:      http://" + config->panel_domain() +
                                                       (local ? ":" + std::to_string(port) : std::string()) +
                                                       "/ (sign in as a host operator)\n")
              << (host.panel_port() ? "  panel:      http://localhost:" + std::to_string(host.panel_port()) + "/ (its own listener)\n"
                                    : std::string())
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

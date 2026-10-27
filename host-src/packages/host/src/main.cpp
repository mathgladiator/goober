// goober-host: serve static content and WASM boxes from one JSON config.
//
//   goober-host [--config host.json] [--local]     run the host
//   goober-host check [--config host.json]         validate the config and exit
//   goober-host token create [--config host.json] [--name NAME] [--claude] [--local]
//   goober-host token list [--config host.json]
//   goober-host token revoke NAME_OR_ID [--config host.json]
//
// --local is for playing on one machine: listen on 127.0.0.1, plain session
// cookies, sign-in codes printed here, and "$domain.localhost" reaching $domain.
// SIGHUP rereads the config; SIGINT and SIGTERM stop the host.

#include <csignal>
#include <cstring>
#include <iostream>
#include <vector>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"

namespace {

constexpr const char* kUsage =
    "usage: goober-host [--config host.json] [--local]\n"
    "       goober-host check [--config host.json]\n"
    "       goober-host token create [--config host.json] [--name NAME] [--claude] [--local]\n"
    "       goober-host token list [--config host.json]\n"
    "       goober-host token revoke NAME_OR_ID [--config host.json]\n";

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

}  // namespace

int main(int argc, char** argv) {
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

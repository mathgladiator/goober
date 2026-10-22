// goober-host: serve static content and WASM boxes from one JSON config.
//
//   goober-host --config host.json
//
// SIGHUP rereads the config; SIGINT and SIGTERM stop the host.

#include <csignal>
#include <cstring>
#include <iostream>

#include "goober/host/host.h"

int main(int argc, char** argv) {
  std::string config_path = "host.json";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--version") {
      std::cout << "goober-host " << goober::host::kVersion << "\n";
      return 0;
    } else {
      std::cerr << "usage: goober-host [--config host.json] [--version]\n";
      return 2;
    }
  }

  // Block the signals before any thread starts, so every thread inherits the
  // mask and only sigwait below receives them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGHUP);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  std::string err;
  auto config = goober::config::load_config(config_path, &err);
  if (!config) {
    std::cerr << "goober-host: " << err << "\n";
    return 1;
  }
  try {
    goober::host::Host host(config, config_path);
    for (const auto& note : host.startup_notes()) std::cerr << "goober-host: " << note << "\n";
    uint16_t port = host.start();
    std::string base = "http://" + config->listen_address + ":" + std::to_string(port);
    std::cerr << "goober-host " << goober::host::kVersion << " listening on " << base << "\n"
              << "  storage:    " << config->storage.string() << "\n"
              << "  operations: " << base << goober::host::kOperationsMcpPath << " (MCP, bearer token)\n";
    for (const auto& [domain, entry] : config->domains) {
      std::cerr << "  domain:     " << domain << " -> tenant " << entry.tenant << "\n";
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

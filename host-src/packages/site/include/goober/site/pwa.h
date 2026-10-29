#pragma once

// Progressive web apps (design/machine/782.PWA.md): a tenant declares apps in
// _apps/$name.json, each over a scope (the whole domain, "/", or a part, like "/app/"). The
// host generates each app's web app manifest, service worker, and registration script, and the
// head tags a page in the scope needs: <!--#include pwa--> in any page, and the built-in base
// template, places them.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/content/content.h"
#include "goober/json/json.h"

namespace goober::site {

struct App {
  std::string name;   // [a-z0-9_-], 1 to 64
  std::string scope;  // "/" or "/x/", ending in "/"
  boost::json::object manifest;  // as written, checked against the manifest fields we know
  // "host": the host's service worker; "own": the tenant's script at worker_src; "none".
  std::string worker = "host";
  std::string worker_src;
  std::string offline_fallback;            // a URL in scope, served when offline and uncached
  std::vector<std::string> precache;       // URLs fetched into the cache at install
  bool push = true;                        // pages in scope may subscribe to push
  bool push_anonymous = false;             // signed-out visitors may subscribe too
  std::string etag;                        // of the definition, so a change updates the worker
};

// Where an app's host-served files live: /goober/pwa/$name/...
std::string app_base(std::string_view name);

bool is_app_name(std::string_view name);

// Parses and checks one definition. Problems that keep it from installing are errors;
// warnings name what browsers want for an install prompt (192 and 512 pixel icons, a name).
std::optional<App> parse_app(std::string_view name, std::string_view json_text, std::string* error,
                             std::vector<std::string>* warnings = nullptr);

// The tenant's apps, from _apps/*.json; definitions that don't parse are skipped.
std::vector<App> apps(const content::ContentStore& store, std::string_view tenant);

// The app whose scope fits a request path, the longest scope winning ("/app" fits "/app/").
std::optional<App> app_for(const std::vector<App>& apps, std::string_view path);

// The web app manifest, as served: the definition's fields, with scope, id, start_url, and
// display filled in.
boost::json::object manifest_for(const App& app);

// What <!--#include pwa--> expands to for the app: the manifest link, theme color, the
// app-capable and touch-icon tags, and the registration script.
std::string head_tags(const App& app);

// The host's service worker for the app, and the script that registers it (and offers
// window.goober.push). push_key is the VAPID public key (base64url), or "" without push.
std::string service_worker(const App& app, std::string_view build);
std::string register_script(const App& app, std::string_view push_key);

}  // namespace goober::site

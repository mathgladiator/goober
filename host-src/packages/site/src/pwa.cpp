#include "goober/site/pwa.h"

#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"

namespace goober::site {

namespace bj = boost::json;

namespace {

// The web app manifest's members (W3C Web Application Manifest, and the extensions browsers
// ship), so a typo is an error rather than a field nobody reads. scope is the app's own.
const std::set<std::string, std::less<>> kManifestFields = {
    "name", "short_name", "description", "start_url", "display", "display_override", "orientation",
    "theme_color", "background_color", "icons", "screenshots", "shortcuts", "categories", "lang", "dir", "id",
    "share_target", "protocol_handlers", "file_handlers", "launch_handler", "prefer_related_applications",
    "related_applications", "handle_links", "edge_side_panel", "note_taking", "iarc_rating_id", "scope_extensions"};

// Paths the host answers itself: a service worker must never cache or intercept them.
const char* const kNeverCached[] = {"/goober/", "/goober-operations", "/identity/", "/oauth/", "/.well-known/", "/mcp",
                                    "/internal/"};

std::string text(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool is_local_url(std::string_view u) {
  return !u.empty() && u.front() == '/' && !u.starts_with("//") && u.find_first_of(" \"'<>\\\r\n\t") == std::string_view::npos;
}

std::string path_of(std::string_view u) { return std::string(u.substr(0, u.find_first_of("?#"))); }

bool in_scope(std::string_view url, std::string_view scope) {
  std::string p = path_of(url);
  return p.starts_with(scope) || p + "/" == scope;
}

bool safe_text(std::string_view s, size_t max) {
  return s.size() <= max && s.find_first_of("\"<>\r\n") == std::string_view::npos;
}

std::string attr(std::string_view s) { return base::html_escape(s); }

}  // namespace

std::string app_base(std::string_view name) { return "/goober/pwa/" + std::string(name) + "/"; }

bool is_app_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

std::optional<App> parse_app(std::string_view name, std::string_view json_text, std::string* error,
                             std::vector<std::string>* warnings) {
  auto bad = [&](std::string why) -> std::optional<App> {
    if (error) *error = std::move(why);
    return std::nullopt;
  };
  if (!is_app_name(name)) return bad("an app's name is 1 to 64 of a-z, 0-9, _ and -");
  std::string err;
  auto v = json::parse(json_text, &err);
  if (!v || !v->is_object()) return bad("the app isn't a JSON object: " + err);
  const auto& o = v->as_object();
  for (const auto& kv : o) {
    if (kv.key() != "scope" && kv.key() != "manifest" && kv.key() != "worker" && kv.key() != "offline" &&
        kv.key() != "push") {
      return bad("unknown field \"" + std::string(kv.key()) + "\": an app has scope, manifest, worker, offline, and push");
    }
  }
  App app;
  app.name = std::string(name);
  app.etag = base::sha256_hex(json::dump(*v)).substr(0, 16);
  app.scope = text(o, "scope");
  if (app.scope.empty()) app.scope = "/";
  if (!is_local_url(app.scope) || app.scope.back() != '/' || app.scope.find_first_of("?#") != std::string::npos ||
      (app.scope != "/" && !base::is_safe_relative_path(app.scope.substr(1, app.scope.size() - 2)))) {
    return bad("scope is \"/\" for the whole domain, or a path like \"/app/\", ending in /");
  }
  for (std::string_view never : kNeverCached) {
    std::string prefix = never.back() == '/' ? std::string(never) : std::string(never) + "/";
    if (app.scope.starts_with(prefix)) return bad("scope " + app.scope + " is under a path the host answers itself");
  }
  if (app.scope.find("/_") != std::string::npos) return bad("scope can't have a segment starting with _");

  const auto* m = o.if_contains("manifest");
  if (!m || !m->is_object()) return bad("manifest is an object of web app manifest fields, with at least name");
  app.manifest = m->as_object();
  for (const auto& kv : app.manifest) {
    if (!kManifestFields.count(kv.key())) {
      return bad("manifest." + std::string(kv.key()) + " isn't a web app manifest field" +
                 (kv.key() == "scope" ? " here: the app's scope is its own field" : ""));
    }
  }
  std::string app_name = text(app.manifest, "name");
  if (app_name.empty() || app_name.size() > 128) return bad("manifest.name is required, at most 128 characters");
  for (const char* k : {"short_name", "theme_color", "background_color"}) {
    if (auto* f = app.manifest.if_contains(k); f && (!f->is_string() || !safe_text(f->as_string(), 64))) {
      return bad(std::string("manifest.") + k + " is text of at most 64 characters, without quotes or <>");
    }
  }
  if (auto d = text(app.manifest, "display"); !d.empty() && d != "fullscreen" && d != "standalone" && d != "minimal-ui" &&
                                              d != "browser") {
    return bad("manifest.display is fullscreen, standalone, minimal-ui, or browser");
  }
  for (const char* k : {"start_url", "id"}) {
    if (auto* f = app.manifest.if_contains(k)) {
      if (!f->is_string() || !is_local_url(f->as_string())) return bad(std::string("manifest.") + k + " is a path on this site, like " + app.scope);
      if (std::string_view(k) == "start_url" && !in_scope(f->as_string(), app.scope)) {
        return bad("manifest.start_url " + std::string(f->as_string()) + " is outside the scope " + app.scope);
      }
    }
  }
  bool has192 = false, has512 = false;
  if (auto* icons = app.manifest.if_contains("icons")) {
    if (!icons->is_array()) return bad("manifest.icons is a list of {src, sizes, type, purpose}");
    for (const auto& i : icons->as_array()) {
      if (!i.is_object() || !is_local_url(text(i.as_object(), "src"))) {
        return bad("each icon has a src on this site, like /attachment/12.png or /icons/192.png");
      }
      std::string sizes = text(i.as_object(), "sizes");
      has192 |= sizes.find("192x192") != std::string::npos || sizes == "any";
      has512 |= sizes.find("512x512") != std::string::npos || sizes == "any";
    }
  }
  if (auto* shortcuts = app.manifest.if_contains("shortcuts")) {
    if (!shortcuts->is_array()) return bad("manifest.shortcuts is a list of {name, url}");
    for (const auto& s : shortcuts->as_array()) {
      if (!s.is_object() || !is_local_url(text(s.as_object(), "url"))) return bad("each shortcut has a url on this site");
    }
  }
  if (warnings) {
    if (!has192 || !has512) warnings->push_back("browsers offer to install only with 192x192 and 512x512 icons");
    if (text(app.manifest, "short_name").empty()) warnings->push_back("short_name names the app under its icon");
  }

  if (const auto* w = o.if_contains("worker")) {
    if (w->is_string() && (w->as_string() == "host" || w->as_string() == "none")) {
      app.worker = std::string(w->as_string());
    } else if (w->is_object() && w->as_object().size() == 1 && w->as_object().contains("src")) {
      app.worker = "own";
      app.worker_src = text(w->as_object(), "src");
      // A worker controls only pages under its own directory (without Service-Worker-Allowed,
      // which content files don't send), so it must sit at or above the scope.
      std::string dir = app.worker_src.substr(0, app.worker_src.rfind('/') + 1);
      if (!is_local_url(app.worker_src) || !app.worker_src.ends_with(".js") || !app.scope.starts_with(dir)) {
        return bad("worker.src is a .js file on this site in the scope's directory or above, like " + app.scope + "sw.js");
      }
    } else {
      return bad("worker is \"host\" (the default), \"none\", or {\"src\": \"/app/sw.js\"}");
    }
  }
  if (const auto* off = o.if_contains("offline")) {
    if (!off->is_object()) return bad("offline is {fallback, precache}");
    for (const auto& kv : off->as_object()) {
      if (kv.key() != "fallback" && kv.key() != "precache") return bad("offline has fallback and precache");
    }
    app.offline_fallback = text(off->as_object(), "fallback");
    if (!app.offline_fallback.empty() && !is_local_url(app.offline_fallback)) return bad("offline.fallback is a path on this site");
    if (auto* pre = off->as_object().if_contains("precache")) {
      if (!pre->is_array() || pre->as_array().size() > 200) return bad("offline.precache is a list of at most 200 paths");
      for (const auto& p : pre->as_array()) {
        if (!p.is_string() || !is_local_url(p.as_string())) return bad("each offline.precache entry is a path on this site");
        app.precache.emplace_back(p.as_string());
      }
    }
  }
  if (const auto* p = o.if_contains("push")) {
    if (p->is_bool()) {
      app.push = p->as_bool();
    } else if (p->is_object()) {
      for (const auto& kv : p->as_object()) {
        if (kv.key() != "anonymous" || !kv.value().is_bool()) return bad("push is true, false, or {\"anonymous\": true}");
      }
      app.push_anonymous = p->as_object().contains("anonymous") && p->as_object().at("anonymous").as_bool();
    } else {
      return bad("push is true, false, or {\"anonymous\": true}");
    }
  }
  return app;
}

std::vector<App> apps(const content::ContentStore& store, std::string_view tenant) {
  std::vector<App> out;
  for (const auto& f : store.list(tenant, "_apps/")) {
    if (!f.ends_with(".json") || f.find('/', 6) != std::string::npos) continue;
    auto file = store.get(tenant, f);
    if (!file) continue;
    std::string err;
    if (auto a = parse_app(f.substr(6, f.size() - 11), file->data, &err)) out.push_back(std::move(*a));
  }
  return out;
}

std::optional<App> app_for(const std::vector<App>& apps, std::string_view path) {
  const App* best = nullptr;
  for (const auto& a : apps) {
    if (in_scope(path, a.scope) && (!best || a.scope.size() > best->scope.size())) best = &a;
  }
  if (!best) return std::nullopt;
  return *best;
}

bj::object manifest_for(const App& app) {
  bj::object m = app.manifest;
  m["scope"] = app.scope;
  if (!m.contains("start_url")) m["start_url"] = app.scope;
  if (!m.contains("id")) m["id"] = app.scope;
  if (!m.contains("display")) m["display"] = "standalone";
  return m;
}

std::string head_tags(const App& app) {
  std::string base = app_base(app.name);
  std::string out = "<link rel=\"manifest\" href=\"" + base + "manifest.webmanifest\">";
  if (auto c = text(app.manifest, "theme_color"); !c.empty()) out += "<meta name=\"theme-color\" content=\"" + attr(c) + "\">";
  std::string title = text(app.manifest, "short_name");
  if (title.empty()) title = text(app.manifest, "name");
  out += "<meta name=\"mobile-web-app-capable\" content=\"yes\"><meta name=\"apple-mobile-web-app-capable\" content=\"yes\">";
  out += "<meta name=\"apple-mobile-web-app-title\" content=\"" + attr(title) + "\">";
  // iOS reads its home-screen icon from the page, not the manifest: the 180 one, else the largest.
  std::string touch;
  int best = 0;
  if (auto* icons = app.manifest.if_contains("icons"); icons && icons->is_array()) {
    for (const auto& i : icons->as_array()) {
      std::string sizes = text(i.as_object(), "sizes");
      int px = std::atoi(sizes.c_str());
      if (sizes.starts_with("180x180")) px = 100000;
      if (px > best) best = px, touch = text(i.as_object(), "src");
    }
  }
  if (!touch.empty()) out += "<link rel=\"apple-touch-icon\" href=\"" + attr(touch) + "\">";
  if (app.worker != "none" || app.push) out += "<script src=\"" + base + "register.js\" defer></script>";
  return out;
}

std::string service_worker(const App& app, std::string_view build) {
  bj::array never;
  for (const char* n : kNeverCached) never.push_back(bj::value(n));
  bj::array precache;
  std::set<std::string> seen;
  auto add = [&](const std::string& u) {
    if (!u.empty() && seen.insert(u).second) precache.push_back(bj::value(u));
  };
  add(text(manifest_for(app), "start_url"));
  add(app.offline_fallback);
  for (const auto& p : app.precache) add(p);
  bj::object config{{"name", app.name},
                    {"title", text(app.manifest, "name")},
                    {"scope", app.scope},
                    {"version", app.etag + "-" + std::string(build)},
                    {"precache", std::move(precache)},
                    {"fallback", app.offline_fallback},
                    {"never", std::move(never)},
                    {"push", app.push},
                    {"icon", ""}};
  if (auto* icons = app.manifest.if_contains("icons"); icons && icons->is_array() && !icons->as_array().empty()) {
    config["icon"] = text(icons->as_array().front().as_object(), "src");
  }
  return "// The goober host's service worker for app \"" + app.name + "\" (design/machine/782.PWA.md § 4).\n"
         "// Generated from _apps/" + app.name + ".json; a change there gives a new version.\n"
         "'use strict';\nconst C = " + json::dump(config) + R"js(;
const CACHE = 'goober-' + C.name + '-' + C.version;
const sameOrigin = (url) => url.origin === self.location.origin;
const never = (path) => C.never.some((p) => path === p || path.startsWith(p.endsWith('/') ? p : p + '/') || path === p);
const inScope = (path) => path.startsWith(C.scope) || path + '/' === C.scope;
const asset = (req, url) => ['style', 'script', 'image', 'font', 'manifest'].includes(req.destination) ||
  url.pathname.startsWith('/attachment/');
// What a person's own browser may keep: never a response the host marked private or no-store.
const keepable = (res) => res && res.ok && res.type === 'basic' &&
  !/(no-store|private)/i.test(res.headers.get('cache-control') || '');

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    // One by one, so a missing page doesn't stop the install.
    await Promise.all(C.precache.map(async (u) => {
      try { const r = await fetch(u, { cache: 'reload', credentials: 'same-origin' }); if (keepable(r)) await cache.put(u, r); } catch (e) {}
    }));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const k of await caches.keys()) if (k.startsWith('goober-' + C.name + '-') && k !== CACHE) await caches.delete(k);
    if (self.registration.navigationPreload) await self.registration.navigationPreload.enable();
    await self.clients.claim();
  })());
});

self.addEventListener('message', (event) => { if (event.data === 'skipWaiting') self.skipWaiting(); });

async function networkFirst(event, fallback) {
  const cache = await caches.open(CACHE);
  try {
    const res = (event.preloadResponse && await event.preloadResponse) || await fetch(event.request);
    if (keepable(res)) cache.put(event.request, res.clone());
    return res;
  } catch (e) {
    const hit = await cache.match(event.request, { ignoreVary: true });
    if (hit) return hit;
    if (fallback) { const f = await cache.match(fallback, { ignoreVary: true }); if (f) return f; }
    return Response.error();
  }
}

async function staleWhileRevalidate(event) {
  const cache = await caches.open(CACHE);
  const hit = await cache.match(event.request, { ignoreVary: true });
  const fresh = fetch(event.request).then((res) => { if (keepable(res)) cache.put(event.request, res.clone()); return res; });
  if (hit) { event.waitUntil(fresh.catch(() => {})); return hit; }
  return fresh;
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (!sameOrigin(url) || never(url.pathname)) return;
  if (req.mode === 'navigate') { if (inScope(url.pathname)) event.respondWith(networkFirst(event, C.fallback)); return; }
  if (asset(req, url)) { event.respondWith(staleWhileRevalidate(event)); return; }
  if (inScope(url.pathname)) event.respondWith(networkFirst(event, ''));
});

// Push (design/machine/782.PWA.md § 5): the host sends {title, body, url, icon, tag, data}.
self.addEventListener('push', (event) => {
  let m = {};
  try { m = event.data ? event.data.json() : {}; } catch (e) { m = { body: event.data ? event.data.text() : '' }; }
  const title = m.title || C.title;
  event.waitUntil(self.registration.showNotification(title, {
    body: m.body || '', icon: m.icon || C.icon || undefined, badge: m.badge || undefined, tag: m.tag || undefined,
    data: { url: m.url || C.scope, data: m.data },
  }));
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const target = new URL((event.notification.data && event.notification.data.url) || C.scope, self.location.origin);
  if (!sameOrigin(target)) return;
  event.waitUntil((async () => {
    for (const c of await self.clients.matchAll({ type: 'window', includeUncontrolled: true })) {
      if (new URL(c.url).pathname === target.pathname && 'focus' in c) return c.focus();
    }
    return self.clients.openWindow(target.href);
  })());
});

// A browser that renews a subscription: tell the host, keeping its topics.
self.addEventListener('pushsubscriptionchange', (event) => {
  event.waitUntil((async () => {
    const key = event.oldSubscription && event.oldSubscription.options && event.oldSubscription.options.applicationServerKey;
    const sub = event.newSubscription || (key && await self.registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: key }));
    if (!sub) return;
    await fetch('/goober/pwa/' + C.name + '/push', { method: 'POST', credentials: 'same-origin',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ subscription: sub.toJSON(), replaces: event.oldSubscription && event.oldSubscription.endpoint }) });
  })());
});
)js";
}

std::string register_script(const App& app, std::string_view push_key) {
  bj::object config{{"name", app.name},
                    {"scope", app.scope},
                    {"worker", app.worker == "host" ? app_base(app.name) + "sw.js" : app.worker == "own" ? app.worker_src : ""},
                    {"push_key", app.push ? std::string(push_key) : std::string()},
                    {"endpoint", app_base(app.name) + "push"}};
  return "// Registers app \"" + app.name + "\"'s service worker, and offers window.goober.push (782.PWA.md).\n"
         "'use strict';\n(() => {\nconst C = " + json::dump(config) + R"js(;
const g = (window.goober = window.goober || {});
const ok = 'serviceWorker' in navigator;
if (ok && C.worker) navigator.serviceWorker.register(C.worker, { scope: C.scope }).catch((e) => console.warn('goober pwa:', e));
const key = () => { const s = C.push_key.replace(/-/g, '+').replace(/_/g, '/'); const b = atob(s + '='.repeat((4 - s.length % 4) % 4));
  return Uint8Array.from(b, (c) => c.charCodeAt(0)); };
const post = (method, body) => fetch(C.endpoint, { method, credentials: 'same-origin',
  headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }).then(async (r) => {
    const j = await r.json().catch(() => ({})); if (!r.ok) throw new Error(j.error || 'push ' + r.status); return j; });
g.push = {
  // Whether this browser can take push for this app (iOS: only once it's on the home screen).
  supported: ok && C.worker !== '' && !!C.push_key && 'PushManager' in window && 'Notification' in window,
  permission: () => ('Notification' in window ? Notification.permission : 'denied'),
  // Asks permission (call it from a tap), subscribes, and tells the host. topics: ["orders"].
  async subscribe(topics) {
    if (!g.push.supported) throw new Error('push is not available here');
    if (await Notification.requestPermission() !== 'granted') throw new Error('notifications were not allowed');
    const reg = await navigator.serviceWorker.ready;
    const sub = (await reg.pushManager.getSubscription()) ||
      await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: key() });
    return post('POST', { subscription: sub.toJSON(), topics: topics || [] });
  },
  async unsubscribe() {
    const reg = await navigator.serviceWorker.ready;
    const sub = await reg.pushManager.getSubscription();
    if (!sub) return { removed: false };
    await post('DELETE', { endpoint: sub.endpoint }).catch(() => {});
    return { removed: await sub.unsubscribe() };
  },
  async subscribed() {
    if (!ok) return false;
    const reg = await navigator.serviceWorker.getRegistration(C.scope);
    return !!(reg && await reg.pushManager.getSubscription());
  },
};
})();
)js";
}

}  // namespace goober::site

#include "goober/boxes/review.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "goober/base/hash.h"
#include "goober/manifest/manifest.h"

namespace goober::boxes {
namespace bj = boost::json;
using manifest::DataStrategy;
using manifest::ExternalService;
using manifest::HttpRoute;
using manifest::Manifest;
using manifest::TableGrant;

namespace {

class Changes {
 public:
  void add(const std::string& area, const std::string& what, const std::string& change, const std::string& cls,
           const std::string& text, const std::string& field = "", const std::string& before = "",
           const std::string& after = "") {
    bj::object o{{"area", area}, {"what", what}, {"change", change}, {"class", cls}, {"text", text}};
    if (!field.empty()) o["field"] = field;
    if (change == "changed") {
      o["before"] = before;
      o["after"] = after;
    }
    list_.push_back(std::move(o));
    if (cls == "widens") widens_.push_back(bj::value(text));
    ++counts_[cls];
  }
  bj::array list_, widens_;
  std::map<std::string, int64_t> counts_{{"widens", 0}, {"narrows", 0}, {"neutral", 0}};
};

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : sep) + s;
  return out;
}
std::string join(const std::set<std::string>& v) { return join(std::vector<std::string>(v.begin(), v.end())); }

std::string requirements_text(const HttpRoute& r) {
  if (r.is_public) return "public";
  std::vector<std::string> clauses;
  for (const auto& c : r.requirements) clauses.push_back(c.empty() ? "any signed-in person" : join(c, " and "));
  return clauses.empty() ? "any signed-in person" : join(clauses, " or ");
}

// Every route the manifest declares, keyed by what it answers.
struct Named {
  std::string kind, key;
  const HttpRoute* route;
};
std::map<std::string, Named> routes_of(const Manifest& m) {
  std::map<std::string, Named> out;
  auto put = [&](const std::string& kind, const std::string& key, const HttpRoute& r) {
    out[kind + " " + key] = Named{kind, key, &r};
  };
  for (const auto& r : m.routes) {
    std::string verb = r.verb;
    for (auto& c : verb) c = char(std::toupper(static_cast<unsigned char>(c)));
    put(r.verb == "subscribe" ? "subscription" : "route", verb + " " + r.pattern.source, r);
  }
  for (const auto& r : m.tools) put("tool", r.name, r);
  for (const auto& r : m.timers) put("timer", r.name, r);
  for (const auto& r : m.internal) {
    std::string verb = r.verb;
    for (auto& c : verb) c = char(std::toupper(static_cast<unsigned char>(c)));
    put("internal route", verb + " " + r.pattern.source, r);
  }
  for (const auto& r : m.identity) put("identity route", r.identity_event, r);
  for (const auto& r : m.mail) put("mail route", r.mail_address, r);
  for (const auto& r : m.results) put("result route", r.name, r);
  for (const auto& r : m.jobs) put("job", r.name, r);
  return out;
}

// Whether outsiders can reach a route of this kind (a request, an agent, a sender, a box);
// the host's own calls (timers, identity events, results, jobs) aren't reach.
bool reachable(const std::string& kind) {
  return kind == "route" || kind == "subscription" || kind == "tool" || kind == "mail route" || kind == "internal route";
}

std::string opt(const std::optional<int64_t>& v) { return v ? std::to_string(*v) : "(default)"; }

void compare_routes(const Manifest* a, const Manifest& b, Changes& out) {
  std::map<std::string, Named> before = a ? routes_of(*a) : std::map<std::string, Named>{};
  std::map<std::string, Named> after = routes_of(b);
  for (const auto& [k, n] : after) {
    auto it = before.find(k);
    std::string what = n.kind + " " + n.key;
    if (it == before.end()) {
      std::string who = reachable(n.kind) ? " (" + requirements_text(*n.route) + ")" : "";
      out.add("routes", what, "added", reachable(n.kind) ? "widens" : "neutral",
              "new " + what + who + (n.kind == "mail route" ? ": may now receive mail at " + n.key : ""));
      continue;
    }
    const HttpRoute& x = *it->second.route;
    const HttpRoute& y = *n.route;
    auto field = [&](const std::string& f, const std::string& bx, const std::string& by, const std::string& cls,
                     const std::string& text) {
      if (bx != by) out.add("routes", what, "changed", cls, text, f, bx, by);
    };
    if (x.name != y.name) field("name", x.name, y.name, "neutral", what + " is now named " + y.name);
    // Who may call it.
    if (x.is_public != y.is_public) {
      field("public", x.is_public ? "true" : "false", y.is_public ? "true" : "false", y.is_public ? "widens" : "narrows",
            y.is_public ? what + " is now public" : what + " now needs " + requirements_text(y));
    } else if (!y.is_public) {
      std::set<std::vector<std::string>> cx(x.requirements.begin(), x.requirements.end());
      std::set<std::vector<std::string>> cy(y.requirements.begin(), y.requirements.end());
      bool added = false, removed = false;
      for (const auto& c : cy) added |= !cx.count(c);
      for (const auto& c : cx) removed |= !cy.count(c);
      if (added || removed) {
        field("requirements", requirements_text(x), requirements_text(y), added ? "widens" : "narrows",
              what + " now admits " + requirements_text(y) + " (was " + requirements_text(x) + ")");
      }
    }
    if (x.agents != y.agents) {
      field("agents", x.agents ? "true" : "false", y.agents ? "true" : "false", y.agents ? "widens" : "narrows",
            y.agents ? "agents' tokens may now call " + what : "agents' tokens may no longer call " + what);
    }
    {
      std::set<std::string> ox(x.cors.begin(), x.cors.end()), oy(y.cors.begin(), y.cors.end());
      bool more = false;
      for (const auto& o : oy) more |= !ox.count(o);
      if (ox != oy) {
        field("cors", join(ox), join(oy), more ? "widens" : "narrows",
              more ? "pages on " + join(oy) + " may now call " + what + " from a browser"
                   : what + " is callable from fewer origins (" + join(oy) + ")");
      }
    }
    {
      std::set<std::string> cx(x.callers.begin(), x.callers.end()), cy(y.callers.begin(), y.callers.end());
      bool more = false;
      for (const auto& c : cy) more |= !cx.count(c);
      if (cx != cy) {
        field("callers", join(cx), join(cy), more ? "widens" : "narrows",
              more ? "boxes " + join(cy) + " may now call " + what : what + " is callable by fewer boxes (" + join(cy) + ")");
      }
    }
    // What browsers may keep, and the rest of the cache.
    if (x.cache.browser_ms != y.cache.browser_ms) {
      field("cache.browser-ms", std::to_string(x.cache.browser_ms), std::to_string(y.cache.browser_ms),
            y.cache.browser_ms > x.cache.browser_ms ? "widens" : "narrows",
            y.cache.browser_ms > x.cache.browser_ms
                ? "browsers and proxies may now keep " + what + "'s answers for " + std::to_string(y.cache.browser_ms / 1000) + " s"
                : "browsers keep " + what + "'s answers for less time (" + std::to_string(y.cache.browser_ms / 1000) + " s)");
    }
    auto cache_text = [](const HttpRoute& r) {
      if (!r.cache.on) return std::string("off");
      return "by " + r.cache.by + (r.cache.wake.empty() ? "" : " (" + join(r.cache.wake) + ")") + ", fresh-ms " +
             std::to_string(r.cache.fresh_ms) + ", check-percent " + std::to_string(r.cache.check_percent);
    };
    field("cache", cache_text(x), cache_text(y), "neutral", what + "'s cache is now " + cache_text(y));
    field("template", x.template_name, y.template_name, "neutral",
          what + (y.template_name.empty() ? " no longer renders a template" : " renders template " + y.template_name));
    field("timeout-ms", opt(x.timeout_ms), opt(y.timeout_ms), "neutral", what + "'s timeout is now " + opt(y.timeout_ms));
    field("body", x.body, y.body, "neutral", what + " now takes body " + (y.body.empty() ? "(none)" : y.body));
    field("max-body-bytes", opt(x.max_body_bytes), opt(y.max_body_bytes), "neutral",
          what + " now takes up to " + opt(y.max_body_bytes) + " bytes");
    field("response", x.response, y.response, "neutral", what + " now answers " + (y.response.empty() ? "(any)" : y.response));
    field("input", x.input, y.input, "neutral", what + " now takes " + (y.input.empty() ? "nothing" : y.input));
    field("description", x.description, y.description, "neutral", what + "'s description changed");
    auto schedule = [](const HttpRoute& r) {
      return r.every_ms ? "every " + std::to_string(*r.every_ms) + " ms" : !r.cron.empty() ? "cron " + r.cron : r.at_field;
    };
    field("schedule", schedule(x), schedule(y), "neutral", what + " now runs " + schedule(y));
    field("mail-attachments", std::to_string(x.mail_attachments), std::to_string(y.mail_attachments), "neutral",
          what + " now takes " + std::to_string(y.mail_attachments) + " attachments");
    field("automatic", x.mail_automatic ? "true" : "false", y.mail_automatic ? "true" : "false", "neutral",
          what + (y.mail_automatic ? " now receives automatic mail" : " no longer receives automatic mail"));
    field("wake", join(x.wake), join(y.wake), "neutral", what + " now wakes on " + join(y.wake));
  }
  for (const auto& [k, n] : before) {
    if (after.count(k)) continue;
    std::string what = n.kind + " " + n.key;
    out.add("routes", what, "removed", reachable(n.kind) ? "narrows" : "neutral", what + " is gone");
  }
}

std::set<std::string> columns(bool all, const std::set<std::string>& cols) {
  return all ? std::set<std::string>{"*"} : cols;
}

void compare_grants(const std::string& strategy, const std::string& table, const TableGrant* a, const TableGrant& b,
                    Changes& out) {
  std::string what = "strategy " + strategy + ", table " + table;
  auto ops = [&](const char* op, bool all_a, const std::set<std::string>& ca, bool all_b,
                 const std::set<std::string>& cb, const std::string& verb) {
    auto x = a ? columns(all_a, ca) : std::set<std::string>{};
    auto y = columns(all_b, cb);
    if (x == y) return;
    std::set<std::string> gained, lost;
    if (!x.count("*")) {
      for (const auto& c : y) {
        if (!x.count(c)) gained.insert(c);
      }
    }
    if (!y.count("*")) {
      for (const auto& c : x) {
        if (!y.count(c)) lost.insert(c);
      }
    }
    auto cols = [&](const std::set<std::string>& s) {
      if (s.count("*")) return "every column of " + table;
      std::vector<std::string> v;
      for (const auto& c : s) v.push_back(table + "." + c);
      return join(v);
    };
    if (!gained.empty()) {
      out.add("data", what, a ? "changed" : "added", "widens", "strategy " + strategy + " may now " + verb + " " + cols(gained),
              op, join(x), join(y));
    }
    if (!lost.empty()) {
      out.add("data", what, "changed", "narrows", "strategy " + strategy + " may no longer " + verb + " " + cols(lost), op,
              join(x), join(y));
    }
  };
  ops("read", a && a->read_all, a ? a->read : std::set<std::string>{}, b.read_all, b.read, "read");
  ops("update", a && a->update_all, a ? a->update : std::set<std::string>{}, b.update_all, b.update, "update");
  auto flag = [&](const char* op, bool x, bool y, const std::string& gained, const std::string& lost) {
    if (x == y) return;
    out.add("data", what, a ? "changed" : "added", y ? "widens" : "narrows",
            "strategy " + strategy + (y ? " may now " + gained : " may no longer " + lost), op, x ? "true" : "false",
            y ? "true" : "false");
  };
  flag("insert", a && a->insert, b.insert, "insert into " + table, "insert into " + table);
  flag("delete", a && a->remove, b.remove, "delete from " + table, "delete from " + table);
}

void compare_data(const Manifest* a, const Manifest& b, Changes& out) {
  std::map<std::string, const DataStrategy*> before, after;
  if (a) {
    for (const auto& d : a->data) before[d.name] = &d;
  }
  for (const auto& d : b.data) after[d.name] = &d;
  auto where = [](const DataStrategy& d) {
    return "database " + d.database + (d.tenant.empty() ? "" : " of tenant " + d.tenant) + " (schema " + d.schema + ")";
  };
  for (const auto& [name, d] : after) {
    auto it = before.find(name);
    const DataStrategy* x = it == before.end() ? nullptr : it->second;
    std::string what = "strategy " + name;
    if (!x) {
      out.add("data", what, "added", d->tables.empty() ? "neutral" : "widens",
              "new strategy " + name + " on " + where(*d) + (d->tables.empty() ? "" : ": see its grants"));
    } else {
      auto field = [&](const std::string& f, const std::string& bx, const std::string& by, const std::string& cls,
                       const std::string& text) {
        if (bx != by) out.add("data", what, "changed", cls, text, f, bx, by);
      };
      field("database", x->database, d->database, "widens", what + " now opens database " + d->database);
      field("tenant", x->tenant, d->tenant, d->tenant.empty() ? "narrows" : "widens",
            d->tenant.empty() ? what + " now opens the request's own tenant's database"
                              : what + " now opens tenant " + d->tenant + "'s database, whatever the request's tenant");
      field("schema", x->schema, d->schema, "neutral", what + " now pins schema " + d->schema);
      field("pattern", x->pattern, d->pattern, "neutral", what + " is now pattern " + d->pattern);
      field("row-limit", std::to_string(x->row_limit), std::to_string(d->row_limit), "neutral",
            what + "'s row-limit is now " + std::to_string(d->row_limit));
      field("step-budget", std::to_string(x->step_budget), std::to_string(d->step_budget), "neutral",
            what + "'s step-budget is now " + std::to_string(d->step_budget));
      field("publish", x->publish, d->publish, "neutral",
            what + (d->publish.empty() ? " no longer publishes" : " now publishes " + d->publish));
    }
    for (const auto& [table, g] : d->tables) {
      const TableGrant* old = nullptr;
      if (x) {
        if (auto t = x->tables.find(table); t != x->tables.end()) old = &t->second;
      }
      compare_grants(name, table, old, g, out);
    }
    if (x) {
      for (const auto& [table, g] : x->tables) {
        if (!d->tables.count(table)) out.add("data", what + ", table " + table, "removed", "narrows", what + " no longer touches " + table);
      }
    }
  }
  for (const auto& [name, d] : before) {
    if (!after.count(name)) out.add("data", "strategy " + name, "removed", "narrows", "strategy " + name + " is gone (" + where(*d) + ")");
  }
}

std::string service_kind(const ExternalService& s) {
  if (s.mail) return s.mail_from.empty() ? "mail from the host's address" : "mail from " + s.mail_from;
  if (s.web) return "the web" + (s.web_hosts.empty() ? std::string() : " (" + join(s.web_hosts) + ")");
  if (s.push) return "push notifications";
  if (s.invites) return "invites" + (s.invite_roles.empty() ? std::string() : " granting " + join(s.invite_roles));
  if (!s.box.empty()) return "box " + s.box;
  if (!s.spec.empty()) return "the API of spec " + s.spec + (s.base.empty() ? "" : " at " + s.base);
  return s.base.empty() ? "an API" : "the API at " + s.base;
}

void compare_services(const Manifest* a, const Manifest& b, Changes& out) {
  std::map<std::string, const ExternalService*> before, after;
  if (a) {
    for (const auto& s : a->external) before[s.name] = &s;
  }
  for (const auto& s : b.external) after[s.name] = &s;
  for (const auto& [name, s] : after) {
    auto it = before.find(name);
    std::string what = "service " + name;
    if (it == before.end()) {
      out.add("services", what, "added", "widens", "may now reach " + service_kind(*s) + " (service " + name + ")");
      for (const auto& [mn, m] : s->methods) {
        out.add("services", what + "." + mn, "added", "widens",
                "may now call " + name + "." + mn + (m.effect.empty() ? "" : " (effect " + m.effect + ")"));
      }
      continue;
    }
    const ExternalService& x = *it->second;
    auto field = [&](const std::string& f, const std::string& bx, const std::string& by, const std::string& cls,
                     const std::string& text) {
      if (bx != by) out.add("services", what, "changed", cls, text, f, bx, by);
    };
    field("mail.from", x.mail_from, s->mail_from, s->mail_from.empty() ? "narrows" : "widens",
          s->mail_from.empty() ? what + " now mails from the host's address" : "may now mail from " + s->mail_from);
    field("mail.from-name", x.mail_from_name, s->mail_from_name, "neutral", what + "'s mail now shows " + s->mail_from_name);
    field("base", x.base, s->base, "widens", what + " now reaches " + s->base);
    field("spec", x.spec, s->spec, "neutral", what + " now pins spec " + s->spec);
    field("box", x.box, s->box, "widens", what + " now calls box " + s->box);
    field("credentials", x.credentials, s->credentials, "neutral", what + " now uses credentials " + s->credentials);
    {
      std::set<std::string> hx(x.web_hosts.begin(), x.web_hosts.end()), hy(s->web_hosts.begin(), s->web_hosts.end());
      bool more = false;
      for (const auto& h : hy) more |= !hx.count(h);
      if (hx != hy) field("web hosts", join(hx), join(hy), more ? "widens" : "narrows", what + " may now read " + join(hy));
    }
    {
      std::set<std::string> rx(x.invite_roles.begin(), x.invite_roles.end()), ry(s->invite_roles.begin(), s->invite_roles.end());
      bool more = false;
      for (const auto& r : ry) more |= !rx.count(r);
      if (rx != ry) field("invite roles", join(rx), join(ry), more ? "widens" : "narrows", "invites may now grant " + join(ry));
    }
    for (const auto& [mn, m] : s->methods) {
      auto old = x.methods.find(mn);
      std::string mw = what + "." + mn;
      if (old == x.methods.end()) {
        out.add("services", mw, "added", "widens", "may now call " + name + "." + mn + (m.effect.empty() ? "" : " (effect " + m.effect + ")"));
        continue;
      }
      const auto& o = old->second;
      if (o.effect != m.effect) {
        bool wider = o.effect == "none" || o.effect.empty();
        out.add("services", mw, "changed", wider ? "widens" : "neutral", name + "." + mn + " now has effect " + m.effect,
                "effect", o.effect, m.effect);
      }
      if (o.budget_calls != m.budget_calls) {
        out.add("services", mw, "changed", m.budget_calls > o.budget_calls ? "widens" : "narrows",
                name + "." + mn + " may now be called " + std::to_string(m.budget_calls) + " times a request (was " +
                    std::to_string(o.budget_calls) + ")",
                "budget", std::to_string(o.budget_calls), std::to_string(m.budget_calls));
      }
      if (o.op != m.op || o.verb != m.verb || o.path != m.path) {
        out.add("services", mw, "changed", "neutral", name + "." + mn + " now calls " + (m.op.empty() ? m.verb + " " + m.path : m.op),
                "operation", o.op.empty() ? o.verb + " " + o.path : o.op, m.op.empty() ? m.verb + " " + m.path : m.op);
      }
    }
    for (const auto& [mn, m] : x.methods) {
      if (!s->methods.count(mn)) out.add("services", what + "." + mn, "removed", "narrows", name + "." + mn + " is gone");
    }
  }
  for (const auto& [name, s] : before) {
    if (!after.count(name)) out.add("services", "service " + name, "removed", "narrows", "no longer reaches " + service_kind(*s));
  }
}

void compare_rest(const Manifest* a, const Manifest& b, const VersionFiles* fa, const VersionFiles& fb, Changes& out) {
  // Domains: which sites the box serves.
  std::set<std::string> dx, dy(b.domains.begin(), b.domains.end());
  if (a) dx.insert(a->domains.begin(), a->domains.end());
  for (const auto& d : dy) {
    if (!dx.count(d)) out.add("box", "domain " + d, "added", "widens", d == "*" ? "now serves every domain" : "now serves " + d);
  }
  for (const auto& d : dx) {
    if (!dy.count(d)) out.add("box", "domain " + d, "removed", "narrows", "no longer serves " + d);
  }
  // Flags: a new switch is neutral; a value it may newly take is more reach.
  std::map<std::string, const Manifest::Flag*> fx, fy;
  if (a) {
    for (const auto& f : a->flags) fx[f.name] = &f;
  }
  for (const auto& f : b.flags) fy[f.name] = &f;
  for (const auto& [n, f] : fy) {
    auto it = fx.find(n);
    if (it == fx.end()) {
      out.add("flags", "flag " + n, "added", "neutral", "new flag " + n + " (default \"" + f->default_value + "\")");
      continue;
    }
    if (it->second->default_value != f->default_value) {
      out.add("flags", "flag " + n, "changed", "neutral", "flag " + n + "'s default is now \"" + f->default_value + "\"",
              "default", it->second->default_value, f->default_value);
    }
    std::set<std::string> vx(it->second->values.begin(), it->second->values.end()), vy(f->values.begin(), f->values.end());
    if (vx != vy) {
      bool more = vy.empty() && !vx.empty();
      for (const auto& v : vy) more |= !vx.empty() && !vx.count(v);
      out.add("flags", "flag " + n, "changed", more ? "widens" : "narrows",
              "flag " + n + " may now be " + (vy.empty() ? std::string("any value") : join(vy)), "values", join(vx), join(vy));
    }
  }
  for (const auto& [n, f] : fx) {
    if (!fy.count(n)) out.add("flags", "flag " + n, "removed", "neutral", "flag " + n + " is gone");
  }
  // Libraries.
  std::set<std::string> lx, ly;
  if (a) {
    for (const auto& l : a->libraries) lx.insert(l.alias + "=" + l.library + "@" + std::to_string(l.major));
  }
  for (const auto& l : b.libraries) ly.insert(l.alias + "=" + l.library + "@" + std::to_string(l.major));
  for (const auto& l : ly) {
    if (!lx.count(l)) out.add("libraries", "library " + l, "added", "neutral", "now imports library " + l);
  }
  for (const auto& l : lx) {
    if (!ly.count(l)) out.add("libraries", "library " + l, "removed", "neutral", "no longer imports library " + l);
  }
  // Templates.
  std::map<std::string, std::string> tx, ty(b.templates.begin(), b.templates.end());
  if (a) tx = a->templates;
  for (const auto& [n, t] : ty) {
    auto it = tx.find(n);
    if (it == tx.end()) out.add("templates", "template " + n, "added", "neutral", "new template " + n);
    else if (it->second != t) out.add("templates", "template " + n, "changed", "neutral", "template " + n + " changed", "text",
                                       base::sha256_hex(it->second).substr(0, 12), base::sha256_hex(t).substr(0, 12));
  }
  for (const auto& [n, t] : tx) {
    if (!ty.count(n)) out.add("templates", "template " + n, "removed", "neutral", "template " + n + " is gone");
  }
  // Limits, mission, the module, and the types document.
  if (a) {
    auto field = [&](const std::string& area, const std::string& f, const std::string& x, const std::string& y,
                     const std::string& text) {
      if (x != y) out.add(area, f, "changed", "neutral", text, f, x, y);
    };
    field("box", "timeout-ms", opt(a->behavior.timeout_ms), opt(b.behavior.timeout_ms),
          "the box's timeout is now " + opt(b.behavior.timeout_ms));
    field("box", "fuel", opt(a->behavior.fuel), opt(b.behavior.fuel), "the box's fuel is now " + opt(b.behavior.fuel));
    field("box", "memory-max-bytes", opt(a->behavior.memory_max_bytes), opt(b.behavior.memory_max_bytes),
          "the box's memory limit is now " + opt(b.behavior.memory_max_bytes));
    field("box", "mission", a->mission, b.mission, "the mission now reads: " + b.mission);
    field("box", "types", a->types, b.types, "the manifest now names types document " + (b.types.empty() ? "(none)" : b.types));
  }
  if (fa) {
    std::string mx = base::sha256_hex(fa->wasm), my = base::sha256_hex(fb.wasm);
    if (mx != my) {
      out.add("module", "box.wasm", "changed", "neutral",
              "the module changed (" + std::to_string(fa->wasm.size()) + " to " + std::to_string(fb.wasm.size()) + " bytes)",
              "sha256", mx, my);
    }
    std::string tx2 = fa->types_text.empty() ? "" : base::sha256_hex(fa->types_text);
    std::string ty2 = fb.types_text.empty() ? "" : base::sha256_hex(fb.types_text);
    if (tx2 != ty2) out.add("types", "types document", "changed", "neutral", "the types document changed", "sha256", tx2, ty2);
  }
}

}  // namespace

std::optional<bj::object> review(const VersionFiles* from, const VersionFiles& to, std::string* error) {
  std::optional<Manifest> a;
  if (from) {
    a = manifest::parse_manifest(from->manifest_text, error);
    if (!a) return std::nullopt;
  }
  auto b = manifest::parse_manifest(to.manifest_text, error);
  if (!b) return std::nullopt;
  Changes out;
  const Manifest* pa = a ? &*a : nullptr;
  compare_routes(pa, *b, out);
  compare_data(pa, *b, out);
  compare_services(pa, *b, out);
  compare_rest(pa, *b, from, to, out);
  bj::object counts;
  for (const auto& [k, n] : out.counts_) counts[k] = n;
  bj::object r{{"box", b->box},
               {"from", from ? bj::value(from->version.empty() ? "(not stored)" : from->version) : bj::value(nullptr)},
               {"to", to.version.empty() ? "(not stored)" : to.version},
               {"new_box", from == nullptr},
               {"module_sha256", base::sha256_hex(to.wasm)},
               {"module_bytes", int64_t(to.wasm.size())},
               {"counts", std::move(counts)},
               {"widens", std::move(out.widens_)},
               {"changes", std::move(out.list_)}};
  return r;
}

}  // namespace goober::boxes

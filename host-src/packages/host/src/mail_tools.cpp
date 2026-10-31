// Mail templates for operators (design/machine/022.MailTemplates.md): list, read, write, remove,
// and preview the templates every host mail is rendered from, per tenant or for all (_host).

#include "goober/host/host.h"
#include "mailer.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

}  // namespace

void Host::register_mail_tools() {
  using mcp::object_property;
  using mcp::object_schema;
  using mcp::string_property;
  namespace mt = mail::templates;

  auto tenant_prop = string_property(
      "A tenant name from the host config, like playground, or _host for the templates every tenant uses unless it "
      "has its own.");
  auto name_prop = string_property(
      "The template: layout (wraps every mail), sign-in-new, sign-in-returning, new-grant, agent-token, or a name "
      "of your own ([a-z][a-z0-9-]*) for boxes to send.");
  auto check_tenant = [this](const std::optional<std::string>& t) {
    if (!t) return false;
    if (*t == mt::kEveryTenant) return true;
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == *t) return true;
    }
    return false;
  };
  // The domain a tenant's mail says it comes from, for previews.
  auto site_of = [this](const std::string& tenant) {
    auto cfg = config_.get();
    for (const auto& [d, e] : cfg->domains) {
      if (e.tenant == tenant) return d;
    }
    return cfg->platform_domain;
  };

  mcp_->add_tool(
      {"mail_template_list",
       "Lists the mail templates in effect for a tenant: every mail the host sends (sign-in codes for new and "
       "returning people, notices about apps and agents) and the layout around them, plus templates of your own. "
       "source says whose copy is used: tenant, every-tenant (_host), or default.",
       object_schema({{"tenant", tenant_prop}}, {"tenant"}),
       [this, check_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!check_tenant(tenant)) return ToolResult::fail("not a tenant in the host config, or _host");
         bj::array out;
         for (const auto& l : mail_templates_->list(*tenant)) {
           bj::object o{{"name", l.name}, {"source", l.source}, {"builtin", l.builtin}};
           if (!l.purpose.empty()) o["purpose"] = l.purpose;
           out.push_back(std::move(o));
         }
         return ToolResult::ok(bj::object{{"templates", std::move(out)}});
       }});

  mcp_->add_tool(
      {"mail_template_get",
       "Reads the template in effect: its subject, text, and html (Mustache), whose copy it is, what it's for, and "
       "the values the host gives it (sample). Start from this to customize one.",
       object_schema({{"tenant", tenant_prop}, {"name", name_prop}}, {"tenant", "name"}),
       [this, check_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name").value_or("");
         if (!check_tenant(tenant)) return ToolResult::fail("not a tenant in the host config, or _host");
         std::string source;
         auto t = mail_templates_->get(*tenant, name, &source);
         if (!t) return ToolResult::fail("no mail template named " + name);
         bj::object o{{"name", name}, {"source", source}, {"subject", t->subject}, {"text", t->text}, {"html", t->html}};
         if (const auto* b = mt::builtin(name)) {
           o["purpose"] = b->purpose;
           o["sample"] = b->sample;
         }
         return ToolResult::ok(std::move(o));
       }});

  mcp_->add_tool(
      {"mail_template_put",
       "Writes a tenant's copy of a mail template (or every tenant's, with _host), or a new template boxes can send "
       "by name. subject and text are plain-text Mustache; html is Mustache whose values are escaped, or \"\" to "
       "make the HTML from the text. The layout's text and html must hold {{{content}}}. Sign-in mails get code, "
       "site, email, and expires_minutes; keep the code in them, and add no links (a link in a sign-in mail is what "
       "a phisher would send). The template is checked by rendering it before it's saved.",
       object_schema({{"tenant", tenant_prop},
                      {"name", name_prop},
                      {"subject", string_property("One line of Mustache, like \"Welcome to {{site}}\".")},
                      {"text", string_property("The plain-text part, in Mustache. Every mail has one.")},
                      {"html", string_property("The HTML part, in Mustache, with inline styles; \"\" to make it from "
                                               "the text. It is placed in the layout's {{{content}}}.")}},
                     {"tenant", "name", "text"}),
       [this, check_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name").value_or("");
         if (!check_tenant(tenant)) return ToolResult::fail("not a tenant in the host config, or _host");
         mt::Template t{arg(args, "subject").value_or(""), arg(args, "text").value_or(""), arg(args, "html").value_or("")};
         std::string err;
         if (!mail_templates_->put(*tenant, name, t, &err)) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"saved", name}, {"tenant", *tenant}});
       }});

  mcp_->add_tool(
      {"mail_template_remove",
       "Removes a tenant's copy of a template (or _host's). A built-in one goes back to every tenant's copy or the "
       "default; one of your own is gone.",
       object_schema({{"tenant", tenant_prop}, {"name", name_prop}}, {"tenant", "name"}),
       [this, check_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name").value_or("");
         if (!check_tenant(tenant)) return ToolResult::fail("not a tenant in the host config, or _host");
         if (!mail_templates_->remove(*tenant, name)) return ToolResult::fail(*tenant + " has no copy of " + name);
         return ToolResult::ok(bj::object{{"removed", name}, {"tenant", *tenant}});
       }});

  // Rendering is a look; sending is a change, so it's its own tool: an observer in the panel, or an
  // agent whose changes wait for approval (035), can preview but never mail anyone.
  auto render_preview = [this, check_tenant, site_of](const bj::object& args, const std::optional<std::string>& to) {
    auto tenant = arg(args, "tenant");
    auto name = arg(args, "name").value_or("");
    if (!check_tenant(tenant)) return ToolResult::fail("not a tenant in the host config, or _host");
    bj::object vars;
    if (const auto* b = mt::builtin(name)) vars = b->sample;
    vars["site"] = site_of(*tenant);
    if (to) vars["email"] = *to;
    if (const auto* v = args.if_contains("vars"); v && v->is_object()) {
      for (const auto& kv : v->as_object()) vars[kv.key()] = kv.value();
    }
    std::string err;
    auto r = mail_templates_->render(*tenant, name, vars, &err);
    if (!r) return ToolResult::fail(err);
    bj::object out{{"subject", r->subject}, {"text", r->text}, {"html", r->html}};
    if (to) {
      if (to->find('@') == std::string::npos || to->find_first_of(" \r\n,;<>") != std::string::npos) {
        return ToolResult::fail("to must be one email address");
      }
      identity::Mail m{*to, "[Test] " + r->subject, r->text};
      m.html = r->html;
      // Logged as the tenant's and marked a test, so mail_activity shows it. (The call itself is
      // an operator action in the access log, like every tool call.)
      if (*tenant != mt::kEveryTenant) m.tenant = *tenant;
      m.test = true;
      mail_->send(std::move(m));
      out["sent_to"] = *to;
    }
    return ToolResult::ok(std::move(out));
  };
  mcp_->add_tool(
      {"mail_template_preview",
       "Renders a template as a tenant would send it, with the sample values (vars overrides any), and returns the "
       "subject, text, and html. Sends nothing; mail_template_send_test sends one.",
       object_schema({{"tenant", tenant_prop},
                      {"name", name_prop},
                      {"vars", object_property("Values to use instead of the samples, like {\"code\": \"123456\"}.")}},
                     {"tenant", "name"}),
       [render_preview](const bj::object& args) { return render_preview(args, std::nullopt); }});
  mcp_->add_tool(
      {"mail_template_send_test",
       "Renders a template as mail_template_preview does and sends it to one address, with [Test] before the subject.",
       object_schema({{"tenant", tenant_prop},
                      {"name", name_prop},
                      {"vars", object_property("Values to use instead of the samples.")},
                      {"to", string_property("The one address to send it to.")}},
                     {"tenant", "name", "to"}),
       [render_preview](const bj::object& args) { return render_preview(args, arg(args, "to")); }});
}

}  // namespace goober::host

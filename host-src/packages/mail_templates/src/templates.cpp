#include "goober/mail_templates/templates.h"

#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/names.h"
#include "goober/render/mustache.h"

namespace goober::mail::templates {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> render_part(std::string_view tmpl, const bj::object& vars, bool html, bool trust_content,
                                       std::string* error) {
  render::TemplateOptions o;
  o.escape_html = html;
  o.max_output_bytes = 1 << 20;
  o.max_partials = 0;
  if (trust_content) o.trusted_html.insert("content");
  return render::render_mustache(tmpl, vars, o, error);
}

std::string first_line(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  return text.substr(start, text.find('\n', start) - start);
}

// A plain-text mail as HTML: escaped, blank lines between paragraphs, single newlines kept.
std::string text_to_html(const std::string& text) {
  std::string out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find("\n\n", start);
    if (end == std::string::npos) end = text.size();
    std::string para = text.substr(start, end - start);
    while (!para.empty() && (para.back() == '\n' || para.back() == '\r' || para.back() == ' ')) para.pop_back();
    size_t lead = para.find_first_not_of("\r\n");
    if (lead != std::string::npos) {
      std::string escaped = base::html_escape(para.substr(lead));
      std::string body;
      for (char c : escaped) {
        if (c == '\r') continue;
        body += c == '\n' ? std::string("<br>\n") : std::string(1, c);
      }
      out += "<p style=\"margin:0 0 16px;\">" + body + "</p>\n";
    }
    start = end + 2;
  }
  return out;
}

// One line, at most 200 bytes: what a subject header can hold.
std::string one_line(std::string s) {
  for (char& c : s) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  }
  size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
  s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
  if (s.size() > 200) s = base::valid_utf8(s.substr(0, 200));
  return s;
}

std::optional<Rendered> wrap(const Template& layout, const std::string& subject, const std::string& text,
                             const std::string& html_body, bj::object vars, std::string* error) {
  Rendered out;
  out.subject = subject;
  vars["subject"] = subject;
  vars["preheader"] = first_line(text);
  vars["content"] = text;
  if (layout.text.empty()) {
    out.text = text;
  } else {
    auto t = render_part(layout.text, vars, false, true, error);
    if (!t) return *error = "layout text: " + *error, std::nullopt;
    out.text = std::move(*t);
  }
  if (!layout.html.empty()) {
    vars["content"] = html_body;
    auto h = render_part(layout.html, vars, true, true, error);
    if (!h) return *error = "layout html: " + *error, std::nullopt;
    out.html = std::move(*h);
  }
  return out;
}

bool valid_tenant(std::string_view tenant) { return tenant == kEveryTenant || base::is_valid_name(tenant); }

}  // namespace

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 63 || name[0] < 'a' || name[0] > 'z') return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

std::optional<Rendered> render_with(const Template& t, const Template& layout, const bj::object& vars_in,
                                    std::string* error) {
  bj::object vars = vars_in;
  for (const char* k : {"site", "email"}) {
    if (!vars.contains(k)) vars[k] = "";
  }
  auto subject = render_part(t.subject, vars, false, false, error);
  if (!subject) return *error = "subject: " + *error, std::nullopt;
  auto text = render_part(t.text, vars, false, false, error);
  if (!text) return *error = "text: " + *error, std::nullopt;
  std::string html_body;
  if (t.html.empty()) {
    html_body = text_to_html(*text);
  } else {
    auto h = render_part(t.html, vars, true, false, error);
    if (!h) return *error = "html: " + *error, std::nullopt;
    html_body = std::move(*h);
  }
  return wrap(layout, one_line(*subject), *text, html_body, std::move(vars), error);
}

Store::Store(fs::path directory) : directory_(std::move(directory)) {}

fs::path Store::file(std::string_view tenant, std::string_view name) const {
  return directory_ / std::string(tenant) / (std::string(name) + ".json");
}

std::optional<Template> Store::load(std::string_view tenant, std::string_view name) const {
  if (!valid_tenant(tenant) || !valid_name(name)) return std::nullopt;
  auto text = base::read_file(file(tenant, name));
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return std::nullopt;
  const auto& o = v->as_object();
  auto part = [&](const char* k) {
    auto* f = o.if_contains(k);
    return f && f->is_string() ? std::string(f->as_string()) : std::string();
  };
  return Template{part("subject"), part("text"), part("html")};
}

std::optional<Template> Store::get(std::string_view tenant, std::string_view name, std::string* source) const {
  std::string ignored;
  if (!source) source = &ignored;
  if (auto t = load(tenant, name)) return *source = "tenant", t;
  if (tenant != kEveryTenant) {
    if (auto t = load(kEveryTenant, name)) return *source = "every-tenant", t;
  }
  if (const auto* b = builtin(name)) return *source = "default", b->defaults;
  return std::nullopt;
}

bool Store::put(std::string_view tenant, std::string_view name, const Template& t, std::string* error) {
  if (!valid_tenant(tenant)) return *error = "not a tenant: " + std::string(tenant), false;
  if (!valid_name(name)) return *error = "a template's name is [a-z][a-z0-9-]*, at most 63", false;
  for (const auto* part : {&t.subject, &t.text, &t.html}) {
    if (part->size() > kMaxPartBytes) return *error = "each part is at most 64 KiB", false;
  }
  bool layout = name == kLayout;
  if (!layout && t.subject.empty()) return *error = "a subject is required", false;
  if (t.subject.find_first_of("\r\n") != std::string::npos) return *error = "the subject is one line", false;
  if (t.text.empty()) return *error = "a text part is required: every mail has one", false;
  const std::pair<const char*, const std::string*> parts[] = {{"subject", &t.subject}, {"text", &t.text}, {"html", &t.html}};
  for (const auto& [label, part] : parts) {
    if (!render::check_mustache(*part, error)) return *error = std::string(label) + ": " + *error, false;
  }
  if (layout) {
    if (t.text.find("{{{content}}}") == std::string::npos ||
        (!t.html.empty() && t.html.find("{{{content}}}") == std::string::npos)) {
      return *error = "a layout puts the mail where it says {{{content}}}, in its text and its html", false;
    }
  }
  // It must render, with the values the host will give it.
  const auto* b = builtin(name);
  bj::object sample = b ? b->sample : bj::object{{"site", "example.com"}, {"email", "ann@example.com"}};
  Template layout_t = layout ? t : *get(tenant, kLayout);
  Template body = layout ? Template{"Hello", "Hello.", "<p>Hello.</p>"} : t;
  if (!render_with(body, layout_t, sample, error)) return false;
  bj::object doc{{"subject", t.subject}, {"text", t.text}, {"html", t.html}};
  fs::create_directories(directory_ / std::string(tenant));
  if (!base::write_file_durable(file(tenant, name), json::dump(doc))) return *error = "couldn't write the template", false;
  return true;
}

bool Store::remove(std::string_view tenant, std::string_view name) {
  if (!valid_tenant(tenant) || !valid_name(name)) return false;
  auto f = file(tenant, name);
  if (!fs::exists(f)) return false;
  return base::remove_file_durable(f);
}

std::vector<Listed> Store::list(std::string_view tenant) const {
  std::vector<Listed> out;
  std::set<std::string> seen;
  for (const auto& b : builtins()) {
    std::string source;
    get(tenant, b.name, &source);
    out.push_back(Listed{b.name, source, true, b.purpose});
    seen.insert(b.name);
  }
  for (std::string_view t : {tenant, kEveryTenant}) {
    if (!valid_tenant(t)) continue;
    for (const auto& f : base::list_files(directory_ / std::string(t))) {
      if (!f.ends_with(".json") || f.find('/') != std::string::npos) continue;
      std::string name = f.substr(0, f.size() - 5);
      if (!valid_name(name) || !seen.insert(name).second) continue;
      std::string source;
      get(tenant, name, &source);
      out.push_back(Listed{name, source, false, ""});
    }
  }
  return out;
}

std::optional<Rendered> Store::render(std::string_view tenant, std::string_view name, const bj::object& vars,
                                      std::string* error) const {
  auto t = get(tenant, name);
  if (!t) return *error = "no mail template named " + std::string(name), std::nullopt;
  return render_with(*t, *get(tenant, kLayout), vars, error);
}

Rendered Store::wrap_text(std::string_view tenant, const std::string& subject, const std::string& text,
                          const bj::object& vars_in) const {
  bj::object vars = vars_in;
  for (const char* k : {"site", "email"}) {
    if (!vars.contains(k)) vars[k] = "";
  }
  std::string err;
  auto out = wrap(*get(tenant, kLayout), subject, text, text_to_html(text), vars, &err);
  // A layout that can't render (it rendered when saved) still lets the mail go, as text.
  return out ? *out : Rendered{subject, text, ""};
}

Rendered Store::wrap_html(std::string_view tenant, const std::string& subject, const std::string& text,
                          const std::string& html_body, const bj::object& vars_in) const {
  bj::object vars = vars_in;
  for (const char* k : {"site", "email"}) {
    if (!vars.contains(k)) vars[k] = "";
  }
  std::string err;
  auto out = wrap(*get(tenant, kLayout), subject, text, html_body, vars, &err);
  return out ? *out : Rendered{subject, text, ""};
}

}  // namespace goober::mail::templates

#pragma once

// Mail templates (design/machine/022.MailTemplates.md): every mail the host sends is rendered
// from a named template with a subject, a text part, and an HTML part, wrapped in a layout.
// The host carries a default for each; an operator may override any of them for one tenant,
// or for every tenant (_host), and add templates of their own that boxes send by name.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::mail::templates {

namespace bj = boost::json;

// The tenant name whose overrides apply to every tenant without its own.
inline constexpr std::string_view kEveryTenant = "_host";
inline constexpr std::string_view kLayout = "layout";
inline constexpr size_t kMaxPartBytes = 64 * 1024;

// Mustache text. The subject and text are rendered as plain text, the HTML with every value
// escaped. In the layout, {{{content}}} is the rendered mail (HTML or text).
struct Template {
  std::string subject;
  std::string text;
  std::string html;  // "": the HTML part is made from the text
};

// A template the host sends itself, with what it is for and the values it is given.
struct Builtin {
  std::string name;
  std::string purpose;
  bj::object sample;  // example values, every one the host gives
  Template defaults;
};
const std::vector<Builtin>& builtins();
const Builtin* builtin(std::string_view name);

// A template's name: [a-z][a-z0-9-]*, at most 63.
bool valid_name(std::string_view name);

struct Rendered {
  std::string subject;
  std::string text;
  std::string html;  // "" when the template has none
};

struct Listed {
  std::string name;
  std::string source;  // "tenant", "every-tenant", or "default"
  bool builtin = false;
  std::string purpose;
};

class Store {
 public:
  explicit Store(std::filesystem::path directory);

  // The template in effect for a tenant: its own, else every tenant's, else the default.
  // nullopt for a name nobody defined. `source` says which.
  std::optional<Template> get(std::string_view tenant, std::string_view name, std::string* source = nullptr) const;
  // Checks and saves an override (or a new template). Refuses a bad name, a part past
  // kMaxPartBytes, a subject on more than one line, Mustache that doesn't parse or render
  // with the sample values, and a layout without {{{content}}}.
  bool put(std::string_view tenant, std::string_view name, const Template& t, std::string* error);
  // Removes an override; a builtin falls back to the default. False when there was none.
  bool remove(std::string_view tenant, std::string_view name);
  std::vector<Listed> list(std::string_view tenant) const;

  // Renders a template and wraps it in the tenant's layout. `vars` are the template's values;
  // site and email (the recipient) are always there.
  std::optional<Rendered> render(std::string_view tenant, std::string_view name, const bj::object& vars,
                                 std::string* error) const;
  // Wraps a mail a box wrote as plain text in the tenant's layout, for its HTML part.
  Rendered wrap_text(std::string_view tenant, const std::string& subject, const std::string& text,
                     const bj::object& vars) const;
  // The same, for a mail whose HTML body the host already rendered (markdown).
  Rendered wrap_html(std::string_view tenant, const std::string& subject, const std::string& text,
                     const std::string& html_body, const bj::object& vars) const;

 private:
  std::filesystem::path file(std::string_view tenant, std::string_view name) const;
  std::optional<Template> load(std::string_view tenant, std::string_view name) const;
  std::filesystem::path directory_;
};

// Renders one template against vars and wraps it in `layout`, without any store.
std::optional<Rendered> render_with(const Template& t, const Template& layout, const bj::object& vars, std::string* error);

}  // namespace goober::mail::templates

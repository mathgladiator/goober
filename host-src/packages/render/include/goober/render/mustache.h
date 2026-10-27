#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "goober/json/json.h"

namespace goober::render {

namespace bj = boost::json;

struct TemplateOptions {
  // Resolves {{> name}} to a template's text. Nothing renders for an unknown name.
  std::function<std::optional<std::string>(std::string_view name)> partial;
  // The names the host fills with HTML it rendered itself, like "body". Only these,
  // looked up at the top of the data, are left unescaped by {{{name}}} and {{& name}};
  // every other value is escaped whatever the braces say, so a template can't be
  // talked into emitting a raw string a person typed (070.UI.md § Templates).
  std::set<std::string, std::less<>> trusted_html;
  int max_partial_depth = 16;
  // Rendering stops with an error past this much output, or this many partials, so
  // branching partials or a huge array can't grow a page without bound.
  size_t max_output_bytes = 8 << 20;
  int max_partials = 2000;
};

// Renders a Mustache template (variables, sections, inverted sections, comments,
// partials, dotted names, and "." for the current item) against JSON data.
// Set-delimiter tags are refused. Returns nullopt, with `error` set, for a malformed
// template.
std::optional<std::string> render_mustache(std::string_view tmpl, const bj::value& data,
                                           const TemplateOptions& options, std::string* error);

// Checks a template without rendering it.
bool check_mustache(std::string_view tmpl, std::string* error);

}  // namespace goober::render

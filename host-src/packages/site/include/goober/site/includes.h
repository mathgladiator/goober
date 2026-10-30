#pragma once

// Shared partials (design/machine/027.Partials.md): <!--#include name--> markers in served
// HTML, expanded from the tenant's _partials/$name.html, one level deep, with the link that
// fits the request marked active.

#include <string>
#include <string_view>
#include <vector>

#include "goober/content/content.h"

namespace goober::site {

struct Expanded {
  std::string html;
  std::vector<std::string> used;     // the partial files spliced in, like "_partials/nav.html"
  std::vector<std::string> missing;  // names with no partial
  std::vector<std::string> errors;   // "name: why" for a partial with parameters that didn't render (spliced as written)
  bool by_auth = false;              // a partial has an @auth variant: the answer depends on the cookie
};

// The names a document's markers include, in order, each once.
std::vector<std::string> include_names(std::string_view html);

// Expands every marker in `html` for a request to `path`. A signed-in visitor gets
// _partials/$name@auth.html when it exists. The partials' own markers stay as they are. A
// marker with parameters (<!--#include card title="Ribs" href="/ribs"-->) renders its partial
// as Mustache over them, each value escaped where {{key}} uses it.
Expanded expand_includes(const content::ContentStore& store, std::string_view tenant, std::string_view html,
                         std::string_view path, bool signed_in);

// Marks, in a partial, the links that fit `path` (data-match, data-match-exact), or those whose
// data-key is `nav_key` when it is set: aria-current="page" and class "active".
std::string mark_active(std::string_view partial, std::string_view path, std::string_view nav_key);

// A partial's name: 1 to 64 of [a-z0-9_-].
bool is_partial_name(std::string_view name);

}  // namespace goober::site

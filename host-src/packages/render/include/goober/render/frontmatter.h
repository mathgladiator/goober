#pragma once

#include <string>
#include <string_view>

#include "goober/json/json.h"

namespace goober::render {

namespace bj = boost::json;

// A page split into its front matter and its body.
struct Document {
  bj::object fields;  // empty when the page has no front matter
  std::string body;
  bool has_front_matter = false;
};

// Splits front matter from a page. Front matter is a block at the very top that opens
// with a line "---" and closes with a line "---" or "...". Each line inside is
// "key: value", where a key is letters, digits, "_", or "-". A value is:
//   - JSON, when it starts with [ or {, like requirements: [["owner"]];
//   - a string in double quotes, read as a JSON string (so \" and \n work), or in
//     single quotes, read as written;
//   - true or false, or an integer, which become a boolean or a number;
//   - anything else, as a string, trimmed.
// "key:" followed by lines of "  - item" is a list of strings. Blank lines and lines
// starting with # are ignored. A page without front matter is all body. On a malformed
// line, `error` is set and the line is skipped, so a page still renders.
Document split_front_matter(std::string_view text, std::string* error = nullptr);

}  // namespace goober::render

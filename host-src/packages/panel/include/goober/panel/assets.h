#pragma once

// The operator panel's browser side (design/machine/028.Panel.md § 3): index.html, panel.css,
// panel.js, and the icon, from packages/panel/assets/, compiled into the binary.

#include <string_view>

namespace goober::panel {

struct Asset {
  std::string_view path;          // "/" for index.html, else "/panel.js" and the like
  std::string_view content_type;
  std::string_view body;
  std::string_view etag;          // a quoted SHA-256 prefix of the body
};

// The asset served at `path`, or nullptr.
const Asset* find(std::string_view path);

}  // namespace goober::panel

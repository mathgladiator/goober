#pragma once

// Reviewing a change to a box (design/machine/005.FirstVersion.md § 9.1): what one version
// changes from another, in one pass a reviewer can read. Every change is classified:
// "widens" (more reach: a new grant, a write allowed, a route made public or easier to call,
// a new mail address, service, or From address, browsers allowed to keep answers, a new flag
// value), "narrows" (less), or "neutral". The widenings come back again as plain sentences.

#include <optional>
#include <string>

#include "goober/json/json.h"

namespace goober::boxes {

// One stored version's files: the manifest's text, the module as binary WASM, and the types
// document's text ("" when there is none).
struct VersionFiles {
  std::string version;  // "" for one not stored yet (an upload under review)
  std::string manifest_text;
  std::string wasm;
  std::string types_text;
};

// The changes from `from` (nullptr: a new box) to `to`:
// {from, to, new_box, changes: [{area, what, change, field?, before?, after?, class, text}],
//  widens: [text], counts: {widens, narrows, neutral}}. nullopt with `error` when either
// manifest doesn't parse.
std::optional<boost::json::object> review(const VersionFiles* from, const VersionFiles& to, std::string* error);

}  // namespace goober::boxes

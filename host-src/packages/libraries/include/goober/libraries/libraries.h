#pragma once

// Host libraries (design/machine/831.Libraries.md): closed, pure, versioned function sets a
// manifest imports ("libraries": {"time": "time@1"}) and a box calls through goober_lib with
// JSON. time@1 is zones, calendar arithmetic, and recurrence; ics@1 is iCalendar and iTIP.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::libraries {

namespace bj = boost::json;

// What a call may read besides its input: the request's clock.
struct Context {
  int64_t now_ms = 0;
  bool used_tzdata = false;  // set by a function that read zone rules
};

// A refusal: code is bad_input, too_large, or limit.
struct Error {
  std::string code, message;
};

struct Function {
  std::string name, description;
  bj::object input, output;  // JSON Schema of the argument and the answer
  bj::array examples;        // [{"in": ..., "out": ...}]
  std::function<bj::object(const bj::object& in, Context& ctx)> run;
};

struct Library {
  std::string name;
  int major = 1;
  std::string description;
  std::vector<Function> functions;
  const Function* function(std::string_view name) const;
};

const std::vector<Library>& all();
const Library* find(std::string_view name, int major);
// "time@1" as ("time", 1), or nullopt.
std::optional<std::pair<std::string, int>> parse_ref(std::string_view ref);

inline constexpr size_t kMaxArgumentBytes = 1 << 20;
// Runs one function: the answer as JSON text, or {"error", "code"} (never throws).
std::string call(const Library& lib, std::string_view function, std::string_view json_in, Context& ctx);

// The contract as served by library_get: functions with their schemas and examples.
bj::object describe(const Library& lib);

// For the mail service, which speaks ics@1 too (831 § 6).
bj::object ics_invite(const bj::object& in, Context& ctx);      // ics.invite
bj::object ics_interpret(const bj::object& in, Context& ctx);   // ics.interpret

}  // namespace goober::libraries

#pragma once

// Per-domain operational controls (design/machine/047.Controls.md): what an operator sets, as
// parsed and checked values, and the matcher that finds the controls a request meets. Pure: no
// clock, no disk, no locks. The gate (gate.h) applies them; the host stores and changes them.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::controls {

namespace bj = boost::json;

enum class Mode { kEnforce, kObserve };
// Whose budget a rate counts against: the client's address (rate_key), the signed-in account (or
// the client when nobody is signed in), or one bucket everybody shares.
enum class By { kClient, kUser, kAll };
// Why a request was refused, or would have been. kTimeout is only ever a would-refuse: an
// observed timeout-ms counts the calls that took longer than it.
enum class Reason { kRate, kConcurrent, kBody, kClosed, kTimeout };
enum class Property { kRate, kConcurrent, kBodyMaxBytes, kTimeoutMs, kClosed };
inline constexpr std::array<Property, 5> kProperties = {Property::kRate, Property::kConcurrent, Property::kBodyMaxBytes,
                                                        Property::kTimeoutMs, Property::kClosed};

const char* mode_name(Mode m);
const char* by_name(By b);
const char* reason_name(Reason r);
const char* property_name(Property p);  // the platform key: rate, concurrent, body-max-bytes, ...

struct Rate {
  double per_minute = 0;
  double burst = 0;
  By by = By::kClient;
};
struct Concurrent {
  int64_t max = 0;         // in flight in the control's scope; 0 is no such limit
  int64_t per_client = 0;  // in flight per client in the scope; 0 is no such limit
};
struct Closed {
  int status = 503;
  int64_t retry_after_s = -1;  // -1: no Retry-After
  std::string message;
};

// One property at one level: not named (the level below decides), "off" (named, and lifted
// here), or a value.
template <class T>
struct Prop {
  bool named = false;
  bool off = false;
  T value{};
  bool on() const { return named && !off; }
};

struct Props {
  Prop<Rate> rate;
  Prop<Concurrent> concurrent;
  Prop<int64_t> body_max_bytes;
  Prop<int64_t> timeout_ms;
  Prop<Closed> closed;
  bool named(Property p) const;
  bool off(Property p) const;
  bool any() const;
};

// What the host allows; a control may only lower these.
struct HostLimits {
  int64_t body_max_bytes = 0;
  int64_t timeout_ms_max = 0;
};

// Ranges, so the tool, the doc, and the tests say the same.
inline constexpr double kRateMinPerMinute = 0.1;
inline constexpr double kRateMaxPerMinute = 1'000'000;
inline constexpr int64_t kConcurrentMax = 100'000;
inline constexpr int64_t kRetryAfterMaxS = 86'400;
inline constexpr size_t kMessageMaxBytes = 300;
inline constexpr size_t kWhyMaxBytes = 300;
inline constexpr size_t kEndpointsMax = 100;
inline constexpr size_t kApisMax = 50;
inline constexpr size_t kHistoryKept = 20;
inline constexpr size_t kRemovedKept = 50;

// Parses a control's properties: only rate, concurrent, body-max-bytes, timeout-ms, and closed,
// each in range or "off". Strict: an unknown key, a wrong type, or a value out of range is an
// error naming `where` and the key.
std::optional<Props> parse_props(const bj::object& set, const HostLimits& limits, const std::string& where,
                                 std::string* error);
// The properties as JSON, in the same shape parse_props reads (defaults filled in).
bj::object props_json(const Props& p);
bj::value property_json(const Props& p, Property which);

// "METHOD /path": METHOD uppercase or *, segments literal, {name} (one segment), or {name*} (the
// rest, possibly empty; last only). GET matches HEAD too.
struct Pattern {
  struct Segment {
    enum class Kind { kLiteral, kOne, kRest } kind = Kind::kLiteral;
    std::string text;  // the literal, or the name
  };
  std::string text;    // as written
  std::string method;  // lowercase, or "" for *
  std::vector<Segment> segments;
  int literals = 0, wildcards = 0;

  static std::optional<Pattern> parse(std::string_view text, std::string* error);
  // method is lowercase, as requests carry it; segments are the decoded path's.
  bool matches(std::string_view method, const std::vector<std::string>& path) const;
};

// An endpoint override's id: [A-Za-z_][A-Za-z0-9_]*, at most 64, and not a name the host gives a
// control (defaults, content, mcp, identity).
bool valid_override_id(std::string_view id, std::string* why);
// An API: box:<box name>, content, mcp, or identity.
bool valid_api(std::string_view api, std::string* why);

enum class Level { kHost, kDomain, kApi, kEndpoint };
const char* level_name(Level l);

struct Control {
  Level level = Level::kDomain;
  std::string id;   // "defaults" for the host's and the domain's, the API's name, or the override's id
  std::string api;  // an API control's API; an override's optional API ("" is any)
  std::optional<Pattern> match;  // overrides only
  Mode mode = Mode::kEnforce;
  Props props;
  size_t order = 0;  // an override's place in the list, the tie-break
};

// host.json's controls.defaults: properties and an optional mode (enforce by default).
std::optional<Control> parse_host_defaults(const bj::object& defaults, const HostLimits& limits, std::string* error);

// One domain's controls, compiled: overrides sorted most specific first, so a request takes the
// first that matches. Immutable once built; the gate swaps whole ones.
struct DomainControls {
  std::string domain, tenant;
  std::optional<Control> host, defaults;
  std::map<std::string, Control, std::less<>> apis;
  std::vector<Control> endpoints;
  // Whether a request's API matters (an API control, or an override that names one): when not,
  // the host needn't work it out.
  bool api_scoped = false;

  bool empty() const { return !host && !defaults && apis.empty() && endpoints.empty(); }
  // The levels a request meets, least specific first: host, domain, API, endpoint (each may be null).
  using Chain = std::array<const Control*, 4>;
  Chain chain(std::string_view method, const std::vector<std::string>& path, std::string_view api) const;
  const Control* find(std::string_view id) const;  // the domain's defaults, an API's, or an override, by id
};

// The stored file (controls/$tenant/$domain.json) compiled, checked whole. A file the host can't
// use is an error, never half applied.
std::optional<DomainControls> compile(const bj::object& file, const std::string& domain, const std::string& tenant,
                                      const std::optional<Control>& host, const HostLimits& limits, std::string* error);

// For one property along a chain: the level that enforces it (the most specific in enforce mode
// that names it), and the observed one (the most specific in observe mode that names it, if it
// is more specific than the enforced one). Either may be null; an "off" value is still returned.
struct Effective {
  const Control* enforced = nullptr;
  const Control* observed = nullptr;
};
Effective effective(const DomainControls::Chain& chain, Property p);

// The id a level is monitored and refused under: the host's defaults count as the domain's.
inline const std::string& monitored_id(const Control& c) {
  static const std::string kDefaults = "defaults";
  return c.level == Level::kHost ? kDefaults : c.id;
}

}  // namespace goober::controls

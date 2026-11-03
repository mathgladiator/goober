#pragma once

// What a control did (design/machine/047.Controls.md § 7): its five-minute rows as a series over
// a window, the window's totals, the effect of its last change (equal spans before and after),
// and plain verdicts an operator can act on. Pure: rows and history in, JSON out.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/controls/spec.h"
#include "goober/operations/traffic.h"

namespace goober::controls {

// One change to a control, as the host keeps it with the control (newest last).
struct Change {
  int64_t at_ms = 0;
  std::string by, why;
  bj::value before, after;  // {mode, set, match?, api?}, or null (created, removed)
};

struct ReportInput {
  std::string id;
  std::string level;          // domain, api, endpoint
  std::optional<Mode> mode;   // nullopt: the control is gone (removed), its rows remain
  std::vector<operations::ControlRow> rows;  // the control's rows, seven days, any order
  std::vector<Change> history;
};

bool valid_report_window(std::string_view window);  // 1h, 12h, or 7d

// Verdict thresholds, named so the doc and the tests agree.
inline constexpr double kShieldShare = 0.5;     // one client holds at least half the refusals
inline constexpr int64_t kShieldMinRefused = 10;
inline constexpr int64_t kManyClients = 10;     // refusals reach at least this many clients
inline constexpr double kTightShare = 0.2;      // or refuse a fifth of what matched from 3 or more
inline constexpr double k5xxShare = 0.05;
inline constexpr int64_t k5xxMinAllowed = 20;

// {id, level, mode, window, step_ms, from_ms, to_ms, points: [{t, matched, allowed, refused,
// rate, concurrent, body, closed, would, n5, peak, p50_us, p99_us}], totals: {matched, allowed,
// refused, rate, concurrent, body, closed, would, refused_ratio, would_ratio, clients_refused,
// top_client_share, would_clients, would_top_share, peak, p50_us, p99_us, ratio_5xx},
// last_change, history (newest first), effect: {changed_at_ms, span_ms, before, after, note?}
// or null, verdicts: [text]}.
bj::object report(const ReportInput& in, std::string_view window, int64_t now_ms);

}  // namespace goober::controls

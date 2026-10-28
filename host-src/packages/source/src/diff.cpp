// Unified diffs: Myers' O((N+M)D) shortest edit script on lines, with hunks of three
// lines of context. Large or wildly different files fall back to replacing the whole file.

#include <algorithm>
#include <string>
#include <vector>

#include "goober/source/source.h"

namespace goober::source {
namespace {

constexpr int kContext = 3;
constexpr int kMaxEdits = 2000;
constexpr size_t kMaxLines = 50000;

std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      out.push_back(text.substr(start));
      break;
    }
    out.push_back(text.substr(start, nl - start + 1));
    start = nl + 1;
  }
  return out;
}

enum class Op { kKeep, kDelete, kInsert };
struct Edit {
  Op op;
  int a, b;  // line indexes in the old and new texts
};

// The shortest edit script, or nullopt when it would take more than kMaxEdits edits.
std::optional<std::vector<Edit>> myers(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b) {
  int n = int(a.size()), m = int(b.size());
  int max = n + m;
  std::vector<int> v(2 * size_t(max) + 2, 0);
  // Each step keeps only the diagonals it can read, -d-1 to d+1: memory grows with D squared,
  // not D times the files' length.
  std::vector<std::vector<int>> trace;
  int offset = max + 1;
  int found = -1;
  for (int d = 0; d <= max && d <= kMaxEdits; ++d) {
    trace.emplace_back(v.begin() + (offset - d - 1), v.begin() + (offset + d + 2));
    for (int k = -d; k <= d; k += 2) {
      int x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) ? v[offset + k + 1] : v[offset + k - 1] + 1;
      int y = x - k;
      while (x < n && y < m && a[x] == b[y]) ++x, ++y;
      v[offset + k] = x;
      if (x >= n && y >= m) {
        found = d;
        break;
      }
    }
    if (found >= 0) break;
  }
  if (found < 0) return std::nullopt;
  // Walk back through the trace to recover the edits.
  std::vector<Edit> edits;
  int x = n, y = m;
  for (int d = found; d > 0; --d) {
    const auto& pv = trace[d];
    auto at = [&](int diag) { return pv[size_t(diag + d + 1)]; };
    int k = x - y;
    int prev_k = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
    int prev_x = at(prev_k);
    int prev_y = prev_x - prev_k;
    while (x > prev_x && y > prev_y) {
      --x, --y;
      edits.push_back({Op::kKeep, x, y});
    }
    if (x == prev_x) {
      --y;
      edits.push_back({Op::kInsert, x, y});
    } else {
      --x;
      edits.push_back({Op::kDelete, x, y});
    }
  }
  while (x > 0 && y > 0) {
    --x, --y;
    edits.push_back({Op::kKeep, x, y});
  }
  std::reverse(edits.begin(), edits.end());
  return edits;
}

void add_line(std::string& out, char sign, std::string_view line) {
  out += sign;
  out += line;
  if (line.empty() || line.back() != '\n') out += "\n\\ No newline at end of file\n";
}

}  // namespace

std::string unified_diff(std::string_view a_text, std::string_view b_text, const std::string& a_name,
                         const std::string& b_name) {
  if (a_text == b_text) return "";
  auto a = lines_of(a_text), b = lines_of(b_text);
  std::string out = "--- " + a_name + "\n+++ " + b_name + "\n";
  std::optional<std::vector<Edit>> edits;
  if (a.size() + b.size() <= kMaxLines) edits = myers(a, b);
  if (!edits) {
    // Too large or too different to be worth a line-by-line diff: the whole file, replaced.
    out += "@@ -" + std::to_string(a.empty() ? 0 : 1) + "," + std::to_string(a.size()) + " +" +
           std::to_string(b.empty() ? 0 : 1) + "," + std::to_string(b.size()) + " @@\n";
    for (auto l : a) add_line(out, '-', l);
    for (auto l : b) add_line(out, '+', l);
    return out;
  }
  // Hunks: runs of changes, with up to kContext kept lines around each, merged when close.
  const auto& e = *edits;
  size_t i = 0;
  while (i < e.size()) {
    while (i < e.size() && e[i].op == Op::kKeep) ++i;
    if (i == e.size()) break;
    size_t start = i >= size_t(kContext) ? i - kContext : 0;
    size_t end = i;
    while (end < e.size()) {
      if (e[end].op != Op::kKeep) {
        ++end;
        continue;
      }
      size_t keeps = 0;
      while (end + keeps < e.size() && e[end + keeps].op == Op::kKeep) ++keeps;
      if (end + keeps == e.size() || keeps > size_t(2 * kContext)) {
        end += std::min(keeps, size_t(kContext));
        break;
      }
      end += keeps;
    }
    int a_start = -1, b_start = -1, a_count = 0, b_count = 0;
    std::string body;
    for (size_t j = start; j < end; ++j) {
      const auto& ed = e[j];
      if (ed.op != Op::kInsert && a_start < 0) a_start = ed.a;
      if (ed.op != Op::kDelete && b_start < 0) b_start = ed.b;
      if (ed.op == Op::kKeep) {
        add_line(body, ' ', a[ed.a]);
        ++a_count, ++b_count;
      } else if (ed.op == Op::kDelete) {
        add_line(body, '-', a[ed.a]);
        ++a_count;
      } else {
        add_line(body, '+', b[ed.b]);
        ++b_count;
      }
    }
    // An empty side starts at the line before (unified diff's convention).
    if (a_start < 0) a_start = (start < e.size() ? e[start].a : int(a.size())) - 1;
    if (b_start < 0) b_start = (start < e.size() ? e[start].b : int(b.size())) - 1;
    out += "@@ -" + std::to_string(a_start + 1) + "," + std::to_string(a_count) + " +" + std::to_string(b_start + 1) +
           "," + std::to_string(b_count) + " @@\n" + body;
    i = end;
  }
  return out;
}

}  // namespace goober::source

#include "goober/base/names.h"

namespace goober::base {

bool is_valid_name(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameLength) return false;
  if (name[0] < 'a' || name[0] > 'z') return false;
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return !name.starts_with("goober_");
}

}  // namespace goober::base

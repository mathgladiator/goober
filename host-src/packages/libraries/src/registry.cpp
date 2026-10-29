#include <chrono>

#include "common.h"

namespace goober::libraries {

Library make_time();
Library make_ics();

const Function* Library::function(std::string_view n) const {
  for (const auto& f : functions) {
    if (f.name == n) return &f;
  }
  return nullptr;
}

const std::vector<Library>& all() {
  static const std::vector<Library> kAll = {make_time(), make_ics()};
  return kAll;
}

const Library* find(std::string_view name, int major) {
  for (const auto& l : all()) {
    if (l.name == name && l.major == major) return &l;
  }
  return nullptr;
}

std::optional<std::pair<std::string, int>> parse_ref(std::string_view ref) {
  auto at = ref.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= ref.size()) return std::nullopt;
  std::string name(ref.substr(0, at));
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
  }
  int major = 0;
  for (char c : ref.substr(at + 1)) {
    if (c < '0' || c > '9' || major > 1000) return std::nullopt;
    major = major * 10 + (c - '0');
  }
  if (major < 1) return std::nullopt;
  return std::make_pair(name, major);
}

std::string call(const Library& lib, std::string_view function, std::string_view json_in, Context& ctx) {
  auto fail = [](const char* code, std::string message) {
    return json::dump(bj::object{{"error", std::move(message)}, {"code", code}});
  };
  const Function* f = lib.function(function);
  if (!f) {
    std::string names;
    for (const auto& x : lib.functions) names += (names.empty() ? "" : ", ") + x.name;
    return fail("unknown_function", lib.name + "@" + std::to_string(lib.major) + " has " + names);
  }
  if (json_in.size() > kMaxArgumentBytes) return fail("too_large", "an argument is at most 1 MiB");
  bj::object in;
  if (!json_in.empty()) {
    std::string err;
    auto v = json::parse(json_in, &err);
    if (!v || !v->is_object()) return fail("bad_input", "the argument is a JSON object");
    in = std::move(v->as_object());
  }
  try {
    return json::dump(f->run(in, ctx));
  } catch (const Error& e) {
    return fail(e.code.c_str(), e.message);
  } catch (const std::exception& e) {
    return fail("bad_input", std::string(function) + " failed: " + e.what());
  }
}

bj::object describe(const Library& lib) {
  bj::array fns;
  for (const auto& f : lib.functions) {
    fns.push_back(bj::object{{"name", f.name}, {"description", f.description}, {"input", f.input}, {"output", f.output},
                             {"examples", f.examples}});
  }
  return bj::object{{"library", lib.name + "@" + std::to_string(lib.major)},
                    {"description", lib.description},
                    {"functions", std::move(fns)}};
}

}  // namespace goober::libraries

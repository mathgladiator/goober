#include "goober/runtime_wasm/engine.h"

#include <any>
#include <chrono>
#include <optional>
#include <set>

#include <wasmtime.hh>

#include "goober/base/encoding.h"
#include "goober/base/random.h"

namespace goober::runtime::wasm {

namespace {

constexpr size_t kMaxLogLines = 1000;
constexpr size_t kMaxLogLineBytes = 4096;
constexpr size_t kMaxHeaders = 32;
constexpr size_t kMaxResponseBytes = 16 << 20;

// Everything one call can see and change, reached from host functions through
// the store's data.
struct CallState {
  const CallInput* input = nullptr;
  base::SeededRandom random{0};
  bool responded = false;
  int32_t status = 0;
  std::string content_type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<LogLine> logs;
  size_t dropped_logs = 0;
};

CallState& state_of(wasmtime::Caller& caller) {
  return *std::any_cast<CallState*>(caller.context().get_data());
}

// The caller's memory as a span, or empty when it has none.
wasmtime::Span<uint8_t> memory_of(wasmtime::Caller& caller) {
  auto ext = caller.get_export("memory");
  if (!ext) return {};
  auto* mem = std::get_if<wasmtime::Memory>(&*ext);
  if (!mem) return {};
  return mem->data(caller.context());
}

// A checked view of box memory, or nullopt for an out-of-bounds range.
std::optional<std::string_view> view(wasmtime::Span<uint8_t> mem, int32_t ptr, int32_t len) {
  if (ptr < 0 || len < 0) return std::nullopt;
  if (uint64_t(ptr) + uint64_t(len) > mem.size()) return std::nullopt;
  return std::string_view(reinterpret_cast<const char*>(mem.data()) + ptr, size_t(len));
}

bool host_owned_header(std::string_view lower) {
  return lower == "set-cookie" || lower == "content-length" || lower == "connection" ||
         lower == "transfer-encoding" || lower == "content-type" || lower.starts_with("goober-");
}

bool valid_header_text(std::string_view s, bool name) {
  if (s.empty() && name) return false;
  for (char c : s) {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return false;
    if (name && (c == ':' || c == ' ')) return false;
  }
  return true;
}

template <typename T>
void must(wasmtime::Result<T> r, const char* what) {
  if (!r) throw std::runtime_error(std::string(what) + ": " + std::string(r.err().message()));
}

}  // namespace

const std::vector<std::string>& abi_imports() {
  static const std::vector<std::string> kImports = {
      "goober_request_size", "goober_request_read", "goober_respond", "goober_header_set",
      "goober_now_ms",       "goober_random_fill",  "goober_log"};
  return kImports;
}

const char* outcome_name(CallResult::Outcome outcome) {
  switch (outcome) {
    case CallResult::Outcome::kOk: return "ok";
    case CallResult::Outcome::kNoResponse: return "no_response";
    case CallResult::Outcome::kFailed: return "failed";
    case CallResult::Outcome::kTrap: return "trap";
    case CallResult::Outcome::kTimeout: return "timeout";
    case CallResult::Outcome::kOutOfFuel: return "out_of_fuel";
    case CallResult::Outcome::kHostError: return "host_error";
  }
  return "unknown";
}

struct CompiledBox::Impl {
  wasmtime::Module module;
  std::vector<std::string> exports;
};

CompiledBox::CompiledBox(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CompiledBox::~CompiledBox() = default;
const std::vector<std::string>& CompiledBox::exports() const { return impl_->exports; }

struct Engine::Impl {
  explicit Impl(const EngineOptions& options) : engine(make_config(options)), linker(engine) {
    define_abi();
    ticker = std::thread([this, tick = options.epoch_tick_ms] {
      while (!stopping.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick));
        engine.increment_epoch();
      }
    });
  }

  ~Impl() {
    stopping.store(true);
    ticker.join();
  }

  static wasmtime::Config make_config(const EngineOptions& options) {
    wasmtime::Config config;
    config.consume_fuel(true);
    config.epoch_interruption(true);
    config.max_wasm_stack(options.max_wasm_stack);
    return config;
  }

  void define_abi() {
    must(linker.func_wrap("goober", "goober_request_size",
                          [](wasmtime::Caller caller) -> int32_t {
                            return int32_t(state_of(caller).input->request_json.size());
                          }),
         "goober_request_size");
    must(linker.func_wrap("goober", "goober_request_read",
                          [](wasmtime::Caller caller, int32_t ptr, int32_t len) -> int32_t {
                            auto& st = state_of(caller);
                            auto mem = memory_of(caller);
                            const auto& doc = st.input->request_json;
                            int32_t n = len < int32_t(doc.size()) ? len : int32_t(doc.size());
                            if (!view(mem, ptr, n)) return -1;
                            std::copy(doc.begin(), doc.begin() + n, mem.data() + ptr);
                            return n;
                          }),
         "goober_request_read");
    must(linker.func_wrap("goober", "goober_respond",
                          [](wasmtime::Caller caller, int32_t status, int32_t type_ptr, int32_t type_len,
                             int32_t body_ptr, int32_t body_len) -> int32_t {
                            auto& st = state_of(caller);
                            auto mem = memory_of(caller);
                            auto type = view(mem, type_ptr, type_len);
                            auto body = view(mem, body_ptr, body_len);
                            if (!type || !body || status < 100 || status > 599) return -1;
                            if (body->size() > kMaxResponseBytes || !valid_header_text(*type, false)) return -1;
                            st.responded = true;
                            st.status = status;
                            st.content_type = std::string(*type);
                            st.body = std::string(*body);
                            return 0;
                          }),
         "goober_respond");
    must(linker.func_wrap("goober", "goober_header_set",
                          [](wasmtime::Caller caller, int32_t name_ptr, int32_t name_len, int32_t value_ptr,
                             int32_t value_len) -> int32_t {
                            auto& st = state_of(caller);
                            auto mem = memory_of(caller);
                            auto name = view(mem, name_ptr, name_len);
                            auto value = view(mem, value_ptr, value_len);
                            if (!name || !value || st.headers.size() >= kMaxHeaders) return -1;
                            std::string lower = base::to_lower(*name);
                            if (!valid_header_text(lower, true) || !valid_header_text(*value, false) ||
                                host_owned_header(lower)) {
                              return -1;
                            }
                            st.headers.emplace_back(std::move(lower), std::string(*value));
                            return 0;
                          }),
         "goober_header_set");
    must(linker.func_wrap("goober", "goober_now_ms",
                          [](wasmtime::Caller caller) -> int64_t { return state_of(caller).input->now_ms; }),
         "goober_now_ms");
    must(linker.func_wrap("goober", "goober_random_fill",
                          [](wasmtime::Caller caller, int32_t ptr, int32_t len) -> int32_t {
                            auto& st = state_of(caller);
                            auto mem = memory_of(caller);
                            if (!view(mem, ptr, len)) return -1;
                            st.random.fill(std::span<uint8_t>(mem.data() + ptr, size_t(len)));
                            return 0;
                          }),
         "goober_random_fill");
    must(linker.func_wrap("goober", "goober_log",
                          [](wasmtime::Caller caller, int32_t level, int32_t ptr, int32_t len) -> int32_t {
                            auto& st = state_of(caller);
                            auto text = view(memory_of(caller), ptr, len);
                            if (!text) return -1;
                            if (st.logs.size() >= kMaxLogLines) {
                              ++st.dropped_logs;
                              return 0;
                            }
                            st.logs.push_back(LogLine{level, std::string(text->substr(0, kMaxLogLineBytes))});
                            return 0;
                          }),
         "goober_log");
  }

  wasmtime::Engine engine;
  wasmtime::Linker linker;
  std::atomic<bool> stopping{false};
  std::thread ticker;
};

Engine::Engine(EngineOptions options) : impl_(std::make_unique<Impl>(options)) {}
Engine::~Engine() = default;

bool Engine::wat_to_wasm(std::string_view wat, std::string* wasm, std::string* error) {
  auto r = wasmtime::wat2wasm(wat);
  if (!r) {
    *error = "invalid WAT: " + std::string(r.err().message());
    return false;
  }
  auto bytes = r.ok();
  wasm->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

std::shared_ptr<const CompiledBox> Engine::compile(std::string_view wasm_or_wat,
                                                   const std::vector<std::string>& route_exports,
                                                   std::string* error) {
  std::string wasm;
  if (!is_binary(wasm_or_wat)) {
    if (!wat_to_wasm(wasm_or_wat, &wasm, error)) return nullptr;
  } else {
    wasm.assign(wasm_or_wat);
  }
  auto compiled = wasmtime::Module::compile(
      impl_->engine, wasmtime::Span<uint8_t>(reinterpret_cast<uint8_t*>(wasm.data()), wasm.size()));
  if (!compiled) {
    *error = "invalid module: " + std::string(compiled.err().message());
    return nullptr;
  }
  auto module = compiled.ok();

  std::set<std::string> allowed(abi_imports().begin(), abi_imports().end());
  for (const auto& imp : module.imports()) {
    std::string mod(imp.module()), name(imp.name());
    if (mod != "goober" || !allowed.count(name)) {
      *error = "import " + mod + "::" + name + " is not part of goober-abi@1; a box may import only goober::" +
               "goober_* host functions";
      return nullptr;
    }
  }
  auto impl = std::make_unique<CompiledBox::Impl>(CompiledBox::Impl{module, {}});
  for (const auto& exp : module.exports()) impl->exports.emplace_back(exp.name());

  // A dry instantiation checks every import's type and every export's shape.
  wasmtime::Store store(impl_->engine);
  must(store.context().set_fuel(1'000'000), "set_fuel");
  store.context().set_epoch_deadline(1000);
  CallState dry;
  CallInput none;
  dry.input = &none;
  store.context().set_data(&dry);
  auto instance = impl_->linker.instantiate(store, module);
  if (!instance) {
    *error = "cannot link the module: " + instance.err().message();
    return nullptr;
  }
  auto inst = instance.ok();
  auto mem = inst.get(store, "memory");
  if (!mem || !std::get_if<wasmtime::Memory>(&*mem)) {
    *error = "the module must export its memory as \"memory\"";
    return nullptr;
  }
  for (const auto& name : route_exports) {
    auto ext = inst.get(store, name);
    auto* func = ext ? std::get_if<wasmtime::Func>(&*ext) : nullptr;
    if (!func) {
      *error = "the module must export a function \"" + name + "\" for its route";
      return nullptr;
    }
    if (!func->typed<std::tuple<>, int32_t>(store)) {
      *error = "\"" + name + "\" must take no arguments and return i32";
      return nullptr;
    }
  }
  if (auto start = inst.get(store, "goober_on_start")) {
    auto* func = std::get_if<wasmtime::Func>(&*start);
    if (!func || !func->typed<std::tuple<>, std::monostate>(store)) {
      *error = "\"goober_on_start\" must be a function taking and returning nothing";
      return nullptr;
    }
  }
  return std::shared_ptr<const CompiledBox>(new CompiledBox(std::move(impl)));
}

CallResult Engine::call(const CompiledBox& box, const std::string& export_name, const CallInput& input,
                        const CallLimits& limits) const {
  CallResult result;
  CallState st;
  st.input = &input;
  st.random = base::SeededRandom(input.seed);

  wasmtime::Store store(impl_->engine);
  store.limiter(limits.memory_max_bytes, -1, 1, -1, -1);
  auto ctx = store.context();
  ctx.set_data(&st);
  uint64_t fuel = uint64_t(limits.fuel);
  if (auto set = ctx.set_fuel(fuel); !set) {
    result.error = "cannot set fuel: " + std::string(set.err().message());
    return result;
  }
  ctx.set_epoch_deadline(uint64_t(limits.timeout_ms));

  auto classify = [&](const wasmtime::TrapError& err) {
    result.error = err.message();
    result.outcome = CallResult::Outcome::kTrap;
    if (const auto* trap = std::get_if<wasmtime::Trap>(&err.data)) {
      if (auto code = trap->code()) {
        if (*code == WASMTIME_TRAP_CODE_INTERRUPT) result.outcome = CallResult::Outcome::kTimeout;
        if (*code == WASMTIME_TRAP_CODE_OUT_OF_FUEL) result.outcome = CallResult::Outcome::kOutOfFuel;
      }
    }
  };
  auto finish = [&] {
    auto left = ctx.get_fuel();
    result.fuel_used = left ? fuel - left.ok() : fuel;
    result.logs = std::move(st.logs);
    if (st.dropped_logs) result.logs.push_back(LogLine{2, std::to_string(st.dropped_logs) + " log lines dropped"});
  };

  auto instance = impl_->linker.instantiate(store, box.impl().module);
  if (!instance) {
    classify(instance.err());
    finish();
    return result;
  }
  auto inst = instance.ok();
  if (auto start = inst.get(store, "goober_on_start")) {
    auto typed = std::get<wasmtime::Func>(*start).typed<std::tuple<>, std::monostate>(store);
    if (typed) {
      auto r = typed.ok().call(store, std::tuple<>{});
      if (!r) {
        classify(r.err());
        result.error = "goober_on_start: " + result.error;
        finish();
        return result;
      }
    }
  }
  auto ext = inst.get(store, export_name);
  auto* func = ext ? std::get_if<wasmtime::Func>(&*ext) : nullptr;
  if (!func) {
    result.outcome = CallResult::Outcome::kHostError;
    result.error = "missing export " + export_name;
    finish();
    return result;
  }
  auto typed = func->typed<std::tuple<>, int32_t>(store);
  if (!typed) {
    result.outcome = CallResult::Outcome::kHostError;
    result.error = "export " + export_name + " has the wrong type";
    finish();
    return result;
  }
  auto r = typed.ok().call(store, std::tuple<>{});
  if (!r) {
    classify(r.err());
    finish();
    return result;
  }
  result.return_code = r.ok();
  finish();
  if (result.return_code != 0) {
    result.outcome = CallResult::Outcome::kFailed;
    result.error = export_name + " returned " + std::to_string(result.return_code);
    return result;
  }
  if (!st.responded) {
    result.outcome = CallResult::Outcome::kNoResponse;
    result.error = export_name + " returned without calling goober_respond";
    return result;
  }
  result.outcome = CallResult::Outcome::kOk;
  result.status = st.status;
  result.content_type = std::move(st.content_type);
  result.body = std::move(st.body);
  result.headers = std::move(st.headers);
  return result;
}

}  // namespace goober::runtime::wasm

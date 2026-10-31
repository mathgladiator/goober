#pragma once

#include <optional>

#include <array>

#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace goober::runtime::wasm {

// The host functions a box may import, all from module "goober" (goober-abi@1).
const std::vector<std::string>& abi_imports();

struct EngineOptions {
  size_t max_wasm_stack = 512 * 1024;
  int epoch_tick_ms = 1;
};

struct CallLimits {
  int64_t timeout_ms = 1000;
  int64_t fuel = 100'000'000;
  int64_t memory_max_bytes = 64 << 20;
};

// What the host hands one call: the request document, plus the time and the
// seed that make the call deterministic.
struct CallInput {
  std::string request_json;
  int64_t now_ms = 0;
  uint64_t seed = 0;
  // With a key, goober_random_fill is ChaCha20 keyed by HMAC-SHA-256(key, seed): the seed alone
  // (in the access log, for replay) can't predict a box's random bytes, and one output doesn't
  // reveal the others. Without one (tests), a seeded xoshiro stream.
  std::optional<std::array<uint8_t, 32>> random_key = std::nullopt;
  // Runs one statement for goober_sql: (strategy, sql, params JSON) to a result JSON.
  // Without it, goober_sql answers {"error": ...}.
  std::function<std::string(std::string_view, std::string_view, std::string_view)> sql = {};
  // Calls a declared external service for goober_call: (service, method, request JSON) to a
  // result JSON (design/machine/015.Network.md). Without it, goober_call answers {"error"}.
  std::function<std::string(std::string_view, std::string_view, std::string_view)> call = {};
  // Calls a declared host library for goober_lib: (library, function, argument JSON) to a result
  // JSON (design/machine/831.Libraries.md). Without it, goober_lib answers {"error"}.
  std::function<std::string(std::string_view, std::string_view, std::string_view)> lib = {};
  // A flag's value for goober_flag (design/machine/039.Flags.md), or nullopt when the manifest
  // doesn't declare it. Without it, every flag is undeclared.
  std::function<std::optional<std::string>(std::string_view)> flag = {};
};

struct LogLine {
  int32_t level = 0;
  std::string text;
};

struct CallResult {
  enum class Outcome { kOk, kNoResponse, kFailed, kTrap, kTimeout, kOutOfFuel, kHostError };
  Outcome outcome = Outcome::kHostError;
  int32_t status = 0;
  std::string content_type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<LogLine> logs;
  uint64_t fuel_used = 0;
  int32_t return_code = 0;
  std::string error;  // why, for anything but kOk
};

const char* outcome_name(CallResult::Outcome outcome);

class Engine;

// A compiled, checked module, shareable across threads.
class CompiledBox {
 public:
  ~CompiledBox();
  struct Impl;
  Impl& impl() const { return *impl_; }
  const std::vector<std::string>& exports() const;

 private:
  friend class Engine;
  explicit CompiledBox(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// One wasmtime engine with fuel metering and epoch interruption, and the
// goober linker. Thread-safe: any thread may compile or call.
class Engine {
 public:
  explicit Engine(EngineOptions options = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Compiles binary WASM, or WAT text when it lacks the binary magic, and checks it:
  // imports only from the goober ABI, a "memory" export, and every export in
  // `route_exports` as a function () -> i32.
  std::shared_ptr<const CompiledBox> compile(std::string_view wasm_or_wat,
                                             const std::vector<std::string>& route_exports, std::string* error);

  // Runs one export in a fresh store and instance.
  CallResult call(const CompiledBox& box, const std::string& export_name, const CallInput& input,
                  const CallLimits& limits) const;

  // True when the bytes start with the binary WASM magic "\0asm"; anything else is treated as WAT.
  static bool is_binary(std::string_view bytes) { return bytes.substr(0, 4) == std::string_view("\0asm", 4); }

  // Converts WAT text to binary WASM.
  static bool wat_to_wasm(std::string_view wat, std::string* wasm, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goober::runtime::wasm

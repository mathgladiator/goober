#pragma once

// A small seeded mutation fuzzer: inputs grow from a seed corpus by inserting a target's
// interesting tokens, repeating ranges (to find depth and quadratic blowups), flipping and
// inserting bytes, and cutting. Runs in the ordinary suite with a few thousand cases per
// target; GOOBER_FUZZ_ITERS asks for more, and the asan preset adds AddressSanitizer.

#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace goober::fuzz {

inline int iterations(int fallback) {
  if (const char* v = std::getenv("GOOBER_FUZZ_ITERS")) return std::max(1, std::atoi(v));
  return fallback;
}

inline std::string mutate(std::mt19937_64& rng, std::string s, const std::vector<std::string>& tokens) {
  int ops = 1 + int(rng() % 6);
  for (int i = 0; i < ops; ++i) {
    size_t at = s.empty() ? 0 : rng() % (s.size() + 1);
    switch (rng() % 7) {
      case 0:
      case 1:
        if (!tokens.empty()) s.insert(at, tokens[rng() % tokens.size()]);
        break;
      case 2: {  // repeat a range: deep nesting, long runs
        if (s.empty()) break;
        size_t len = 1 + rng() % std::min<size_t>(16, s.size() - std::min(at, s.size() - 1));
        size_t from = std::min(at, s.size() - 1);
        std::string piece = s.substr(from, len);
        int times = 1 + int(rng() % 400);
        std::string rep;
        for (int k = 0; k < times && rep.size() < 20000; ++k) rep += piece;
        s.insert(from, rep);
        break;
      }
      case 3:
        if (!s.empty()) s[std::min(at, s.size() - 1)] ^= char(1 << (rng() % 8));
        break;
      case 4:
        s.insert(at, 1, char(rng() % 256));
        break;
      case 5:
        if (!s.empty()) s.erase(std::min(at, s.size() - 1), 1 + rng() % 8);
        break;
      case 6:
        if (s.size() > 4) s.resize(rng() % s.size());
        break;
    }
    if (s.size() > 100000) s.resize(100000);
  }
  return s;
}

// Runs `target` on mutations of the corpus and fails if any single input takes too long.
template <typename F>
void run(const char* name, const std::vector<std::string>& corpus, const std::vector<std::string>& tokens, int iters,
         F target, int64_t slowest_ms = 3000) {
  std::mt19937_64 rng(0x600be4ull ^ std::hash<std::string>{}(name));
  std::vector<std::string> pool = corpus;
  for (int i = 0; i < iters; ++i) {
    std::string input = mutate(rng, pool[rng() % pool.size()], tokens);
    auto start = std::chrono::steady_clock::now();
    target(input);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ms > slowest_ms) {
      ADD_FAILURE() << name << ": an input of " << input.size() << " bytes took " << ms << " ms: "
                    << input.substr(0, 120);
      return;
    }
    // Keep some survivors, so inputs grow more interesting over the run.
    if (rng() % 8 == 0 && input.size() < 20000) pool.push_back(std::move(input));
    if (pool.size() > 200) pool.erase(pool.begin() + long(corpus.size()) + long(rng() % (pool.size() - corpus.size())));
  }
}

}  // namespace goober::fuzz

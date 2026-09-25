// Tiny test harness so the project doesn't need gtest.
#pragma once

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"
#include "elaborate.h"
#include "sim.h"

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                          \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto va_ = (a);                                                                        \
    auto vb_ = (b);                                                                        \
    if (!(va_ == vb_)) {                                                                   \
      std::fprintf(stderr, "%s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                   (long long)va_, (long long)vb_);                                        \
      g_failures++;                                                                        \
    }                                                                                      \
  } while (0)

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

#define TEST(name)                                                              \
  static void name();                                                           \
  static const bool name##_registered = (registry().push_back({#name, name}), true); \
  static void name()

inline int run_all() {
  for (const auto& t : registry()) {
    int before = g_failures;
    try {
      t.fn();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s threw: %s\n", t.name, e.what());
      g_failures++;
    }
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}

inline lf::Graph build(const std::string& src) {
  auto mods = lf::parse_text(src, "test.sv");
  return lf::elaborate(mods.back());
}

// Evaluates one input assignment. Returns output values by port name, and
// flop next-state values as "<reg>.d".
inline std::map<std::string, uint64_t> eval(const lf::Graph& g, const std::map<std::string, uint64_t>& in,
                                            const std::map<std::string, uint64_t>& state = {}) {
  std::map<uint32_t, std::pair<std::string, int>> src_of;
  for (const auto& b : g.inputs) src_of[b.node] = {b.name, b.bit};
  std::map<uint32_t, std::pair<std::string, int>> q_of;
  for (const auto& f : g.flops) q_of[f.q] = {f.name, f.bit};
  std::vector<uint64_t> words;
  for (uint32_t i = 2; i < g.size(); i++) {
    lf::Op op = g.nodes[i].op;
    if (op == lf::Op::Input) {
      auto [name, bit] = src_of[i];
      auto it = in.find(name);
      words.push_back(it != in.end() && ((it->second >> bit) & 1) ? ~0ull : 0);
    } else if (op == lf::Op::FlopQ) {
      auto [name, bit] = q_of[i];
      auto it = state.find(name);
      words.push_back(it != state.end() && ((it->second >> bit) & 1) ? ~0ull : 0);
    }
  }
  auto v = lf::simulate(g, words, 1);
  std::map<std::string, uint64_t> out;
  for (const auto& o : g.outputs)
    if (v[o.node] & 1) out[o.name] |= uint64_t(1) << o.bit;
    else out[o.name] |= 0;
  for (const auto& f : g.flops)
    if (v[f.d] & 1) out[f.name + ".d"] |= uint64_t(1) << f.bit;
    else out[f.name + ".d"] |= 0;
  return out;
}

inline bool throws_with(const std::string& src, const std::string& needle) {
  try {
    build(src);
  } catch (const std::exception& e) {
    if (std::string(e.what()).find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  wrong error: %s (wanted '%s')\n", e.what(), needle.c_str());
    return false;
  }
  std::fprintf(stderr, "  no error (wanted '%s')\n", needle.c_str());
  return false;
}

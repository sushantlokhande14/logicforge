#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ir.h"

namespace lf {

// 64 input patterns per machine word: each node's value is a vector of words.
// values[node * words + w]
std::vector<uint64_t> simulate(const Graph& g, const std::vector<uint64_t>& source_words, size_t words);

struct EquivResult {
  bool equal = true;
  bool exhaustive = false;
  uint64_t patterns = 0;
  std::string detail;  // first mismatch, human readable
};

// Compares two versions of the same module. Ports are matched by name and bit,
// flops by register name and bit (flop outputs are treated as free inputs, flop
// D pins as extra outputs). With <= 16 free inputs every combination is tried,
// which makes the answer a proof; otherwise it is `random_words * 64` patterns.
EquivResult check_equivalence(const Graph& ref, const Graph& impl, size_t random_words = 256, uint64_t seed = 1);

}  // namespace lf

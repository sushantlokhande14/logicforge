#pragma once

#include <string>
#include <vector>

#include "ir.h"

namespace lf {

// Every pass rebuilds the graph in one forward sweep and leaves it valid.
void const_prop(Graph& g);  // fold gates with constant inputs
void simplify(Graph& g);    // Boolean identities: x&x, x&~x, ~~x, absorption, mux rules
void strash(Graph& g);      // structural hashing: merge identical gates (CSE)
void dce(Graph& g);         // drop logic (and flops) that no output depends on
void balance(Graph& g);     // rebuild AND/OR/XOR chains as minimum-depth trees
// Merge gates that compute the same function. Candidates come from random
// simulation; a merge only happens after an exhaustive check over the combined
// support, so it is exact. Gates with more than max_support inputs are skipped.
void sweep(Graph& g, int max_support);

struct PassOptions {
  bool balance = true;
  bool sweep = true;
  int sweep_support = 10;
  int max_iters = 4;
  bool verify_each = false;
  std::vector<std::string> skip;  // pass names to leave out, for bisecting a bad pass
};

struct PassLog {
  std::string name;
  long gates_removed = 0;
  double ms = 0;
  int runs = 0;
};

// Runs the pass pipeline until gates and depth stop improving.
void optimize(Graph& g, const PassOptions& o, std::vector<PassLog>* log = nullptr);

void merge_logs(std::vector<PassLog>& into, const std::vector<PassLog>& from);

}  // namespace lf

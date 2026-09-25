#pragma once

#include <vector>

#include "ir.h"
#include "passes.h"
#include "pool.h"

namespace lf {

struct FlowOptions {
  int threads = 1;
  bool regions = true;  // false: run the passes on the whole graph in one go
  PassOptions passes;
};

struct FlowStats {
  size_t regions = 0;
  size_t dead_regions = 0;
  size_t dead_gates = 0;  // gates in regions nothing reads, dropped without optimizing
  size_t largest_region = 0;
  double ms_partition = 0, ms_optimize = 0, ms_merge = 0, ms_cleanup = 0;
  std::vector<PassLog> passes;
  std::vector<StealPool::WorkerStats> workers;
};

// Splits the graph into independent logic regions (groups of gates that share no
// gate with any other group), optimizes each region on the pool, stitches the
// results back together and finishes with a cheap global cleanup.
// Output is identical for any thread count.
Graph optimize_design(const Graph& g, const FlowOptions& o, FlowStats* st = nullptr);

}  // namespace lf

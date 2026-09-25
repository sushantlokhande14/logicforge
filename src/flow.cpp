#include "flow.h"

#include <algorithm>
#include <chrono>
#include <numeric>

#include "builder.h"

namespace lf {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Region {
  std::vector<uint32_t> nodes;  // gate ids, topological order
  std::vector<uint32_t> roots;  // gates read by outputs or flop D pins
};

struct RegionResult {
  Graph sub;
  std::vector<uint32_t> srcs;  // sub input k is global source srcs[k]
  std::vector<PassLog> log;
};

uint32_t find(std::vector<uint32_t>& p, uint32_t x) {
  while (p[x] != x) {
    p[x] = p[p[x]];
    x = p[x];
  }
  return x;
}

// Union-find over gate->gate edges. Sources (inputs, flop outputs, constants)
// don't join regions: two cones that only share an input are still independent.
std::vector<Region> partition(const Graph& g, std::vector<uint32_t>& pos) {
  std::vector<uint32_t> parent(g.size());
  std::iota(parent.begin(), parent.end(), 0);
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.at(i);
    for (int j = 0; j < op_arity(n.op); j++) {
      uint32_t f = n.in[j];
      if (!is_gate(g.at(f).op)) continue;
      uint32_t a = find(parent, i), b = find(parent, f);
      if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
  }
  std::vector<int> rid(g.size(), -1);
  std::vector<Region> regions;
  for (uint32_t i = 2; i < g.size(); i++) {
    if (!is_gate(g.at(i).op)) continue;
    uint32_t r = find(parent, i);
    if (rid[r] < 0) {
      rid[r] = (int)regions.size();
      regions.emplace_back();
    }
    rid[i] = rid[r];
    pos[i] = uint32_t(regions[rid[i]].nodes.size());
    regions[rid[i]].nodes.push_back(i);
  }
  std::vector<char> seen(g.size(), 0);
  auto root = [&](uint32_t x) {
    if (!is_gate(g.at(x).op) || seen[x]) return;
    seen[x] = 1;
    regions[rid[x]].roots.push_back(x);
  };
  for (const auto& o : g.outputs) root(o.node);
  for (const auto& f : g.flops) root(f.d);
  return regions;
}

RegionResult optimize_region(const Graph& g, const Region& r, const std::vector<uint32_t>& pos,
                             const PassOptions& po) {
  RegionResult res;
  Graph& s = res.sub;
  s.module = g.module;
  // Sources this region reads, sorted, become the sub-graph's inputs. Gates are
  // renumbered with pos[] from partition(), so no per-thread lookup table is
  // needed. (A dense table sized to the whole design, per thread, made -j16
  // slower than -j8: every thread page-faulted megabytes of zeros.)
  for (uint32_t x : r.nodes) {
    const Node& n = g.at(x);
    for (int j = 0; j < op_arity(n.op); j++)
      if (n.in[j] >= 2 && !is_gate(g.at(n.in[j]).op)) res.srcs.push_back(n.in[j]);
  }
  std::sort(res.srcs.begin(), res.srcs.end());
  res.srcs.erase(std::unique(res.srcs.begin(), res.srcs.end()), res.srcs.end());
  s.nodes.reserve(2 + res.srcs.size() + r.nodes.size());
  for (size_t k = 0; k < res.srcs.size(); k++) s.inputs.push_back({"", (int)k, s.add(Op::Input)});

  const uint32_t base = uint32_t(2 + res.srcs.size());
  auto get = [&](uint32_t f) -> uint32_t {
    if (f < 2) return f;
    if (is_gate(g.at(f).op)) return base + pos[f];
    return 2 + uint32_t(std::lower_bound(res.srcs.begin(), res.srcs.end(), f) - res.srcs.begin());
  };
  for (uint32_t x : r.nodes) {
    const Node& n = g.at(x);
    uint32_t in[3] = {0, 0, 0};
    for (int j = 0; j < op_arity(n.op); j++) in[j] = get(n.in[j]);
    s.add(n.op, in[0], in[1], in[2]);
  }
  for (size_t k = 0; k < r.roots.size(); k++) s.outputs.push_back({"", (int)k, get(r.roots[k])});
  optimize(s, po, &res.log);
  return res;
}

void cleanup(Graph& g) {
  strash(g);
  dce(g);
}

}  // namespace

Graph optimize_design(const Graph& g, const FlowOptions& o, FlowStats* st) {
  FlowStats local;
  FlowStats& S = st ? *st : local;

  if (!o.regions) {
    auto t = Clock::now();
    Graph out = g;
    optimize(out, o.passes, &S.passes);
    S.ms_optimize = ms_since(t);
    t = Clock::now();
    cleanup(out);
    S.ms_cleanup = ms_since(t);
    return out;
  }

  auto t = Clock::now();
  std::vector<uint32_t> pos(g.size(), 0);
  std::vector<Region> regions = partition(g, pos);
  std::vector<size_t> live;
  for (size_t i = 0; i < regions.size(); i++) {
    S.largest_region = std::max(S.largest_region, regions[i].nodes.size());
    if (regions[i].roots.empty()) {  // nothing reads it: skip entirely
      S.dead_regions++;
      S.dead_gates += regions[i].nodes.size();
    } else {
      live.push_back(i);
    }
  }
  S.regions = regions.size();
  // biggest first, so the long tasks don't start last
  std::stable_sort(live.begin(), live.end(),
                   [&](size_t a, size_t b) { return regions[a].nodes.size() > regions[b].nodes.size(); });
  S.ms_partition = ms_since(t);

  t = Clock::now();
  std::vector<RegionResult> results(regions.size());
  StealPool pool(o.threads);
  pool.run(live, [&](size_t r, int) { results[r] = optimize_region(g, regions[r], pos, o.passes); });
  S.workers = pool.stats();
  S.ms_optimize = ms_since(t);

  // Stitch regions back together. Hash-consing while we copy catches identical
  // logic that ended up in two different regions (regions can't see each other).
  t = Clock::now();
  Graph out = g.empty_like();
  Builder hb(out, false, false, true, g.size());
  std::vector<uint32_t> src_map(g.size(), kNone), root_map(g.size(), kNone);
  src_map[0] = kFalse;
  src_map[1] = kTrue;
  for (uint32_t i = 2; i < g.size(); i++)
    if (is_source(g.at(i).op)) src_map[i] = out.add(g.at(i).op);
  for (size_t r = 0; r < regions.size(); r++) {  // region order, not finish order: keeps output deterministic
    if (regions[r].roots.empty()) continue;
    const RegionResult& res = results[r];
    const Graph& s = res.sub;
    std::vector<uint32_t> m(s.size(), kNone);
    m[0] = kFalse;
    m[1] = kTrue;
    for (const auto& b : s.inputs) m[b.node] = src_map[res.srcs[b.bit]];
    for (uint32_t j = 2; j < s.size(); j++) {
      const Node& n = s.at(j);
      if (n.op == Op::Input) continue;
      m[j] = hb.make(n.op, m[n.in[0]], m[n.in[1]], m[n.in[2]]);
    }
    for (size_t k = 0; k < regions[r].roots.size(); k++) root_map[regions[r].roots[k]] = m[s.outputs[k].node];
    merge_logs(S.passes, res.log);
  }
  auto resolve = [&](uint32_t x) {
    if (x < 2) return x;
    return is_source(g.at(x).op) ? src_map[x] : root_map[x];
  };
  for (const auto& b : g.inputs) out.inputs.push_back({b.name, b.bit, src_map[b.node]});
  for (const auto& ob : g.outputs) out.outputs.push_back({ob.name, ob.bit, resolve(ob.node)});
  for (const auto& f : g.flops) out.flops.push_back({f.name, f.bit, f.clock, src_map[f.q], resolve(f.d)});
  S.ms_merge = ms_since(t);

  // flops nobody reads are only visible globally
  t = Clock::now();
  dce(out);
  S.ms_cleanup = ms_since(t);
  return out;
}

}  // namespace lf

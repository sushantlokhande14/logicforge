#include "passes.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <queue>
#include <unordered_map>

#include "builder.h"

namespace lf {

namespace {

constexpr uint32_t kNone = kNoNode;

bool associative(Op op) { return commutative(op); }

// Copies g into out in order. Sources are copied as-is, gates go through fn,
// which gets the old id and the already-remapped fanins.
template <class Fn>
void rebuild(const Graph& g, Graph& out, std::vector<uint32_t>& map, Fn&& fn) {
  map.assign(g.size(), kNone);
  map[0] = kFalse;
  map[1] = kTrue;
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    if (is_source(n.op)) {
      map[i] = out.add(n.op);
      continue;
    }
    auto m = [&](uint32_t x) { return map[x]; };
    map[i] = fn(i, n.op, m(n.in[0]), m(n.in[1]), m(n.in[2]));
  }
  for (const auto& b : g.inputs) out.inputs.push_back({b.name, b.bit, map[b.node]});
  for (const auto& o : g.outputs) out.outputs.push_back({o.name, o.bit, map[o.node]});
  for (const auto& f : g.flops) out.flops.push_back({f.name, f.bit, f.clock, map[f.q], map[f.d]});
}

void rebuild_with(Graph& g, bool fold, bool simp, bool hash) {
  Graph out = g.empty_like();
  Builder mk(out, fold, simp, hash, g.size());
  std::vector<uint32_t> map;
  rebuild(g, out, map, [&](uint32_t, Op op, uint32_t a, uint32_t b, uint32_t c) { return mk.make(op, a, b, c); });
  g = std::move(out);
}

uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t eval_word(Op op, uint64_t a, uint64_t b, uint64_t c) {
  switch (op) {
    case Op::Not: return ~a;
    case Op::And: return a & b;
    case Op::Or: return a | b;
    case Op::Xor: return a ^ b;
    case Op::Mux: return (a & b) | (~a & c);
    default: return 0;
  }
}

// Pattern for truth-table variable k in word w: the classic 0xAAAA.., 0xCCCC.. masks.
uint64_t var_word(int k, size_t w) {
  static const uint64_t low[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                  0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  if (k < 6) return low[k];
  return (w >> (k - 6)) & 1 ? ~0ull : 0ull;
}

}  // namespace

void const_prop(Graph& g) { rebuild_with(g, true, false, false); }
void simplify(Graph& g) { rebuild_with(g, false, true, false); }
void strash(Graph& g) { rebuild_with(g, false, false, true); }

void dce(Graph& g) {
  std::vector<char> live(g.size(), 0);
  std::vector<int> flop_of(g.size(), -1);
  for (size_t f = 0; f < g.flops.size(); f++) flop_of[g.flops[f].q] = (int)f;
  std::vector<uint32_t> stack;
  for (const auto& o : g.outputs) stack.push_back(o.node);
  while (!stack.empty()) {
    uint32_t x = stack.back();
    stack.pop_back();
    if (live[x]) continue;
    live[x] = 1;
    const Node& n = g.nodes[x];
    if (n.op == Op::FlopQ) stack.push_back(g.flops[flop_of[x]].d);
    for (int j = 0; j < op_arity(n.op); j++) stack.push_back(n.in[j]);
  }

  Graph out = g.empty_like();
  std::vector<uint32_t> map(g.size(), kNone);
  map[0] = kFalse;
  map[1] = kTrue;
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    if (n.op == Op::Input) map[i] = out.add(Op::Input);  // ports stay even if unused
    else if (!live[i]) continue;
    else if (n.op == Op::FlopQ) map[i] = out.add(Op::FlopQ);
    else map[i] = out.add(n.op, map[n.in[0]], map[n.in[1]], map[n.in[2]]);
  }
  for (const auto& b : g.inputs) out.inputs.push_back({b.name, b.bit, map[b.node]});
  for (const auto& o : g.outputs) out.outputs.push_back({o.name, o.bit, map[o.node]});
  for (const auto& f : g.flops)
    if (live[f.q]) out.flops.push_back({f.name, f.bit, f.clock, map[f.q], map[f.d]});
  g = std::move(out);
}

void balance(Graph& g) {
  auto fo = fanout_counts(g);
  // A same-op child with a single fanout is folded into its parent's tree.
  std::vector<char> inner(g.size(), 0);
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    if (!associative(n.op)) continue;
    for (int j = 0; j < 2; j++) {
      uint32_t f = n.in[j];
      if (g.nodes[f].op == n.op && fo[f] == 1) inner[f] = 1;
    }
  }

  Graph out = g.empty_like();
  std::vector<int> lv;  // levels of nodes in `out`, filled lazily
  auto level = [&](uint32_t x) {
    while (lv.size() < out.size()) {
      const Node& n = out.nodes[lv.size()];
      int m = 0;
      for (int j = 0; j < op_arity(n.op); j++) m = std::max(m, lv[n.in[j]]);
      lv.push_back(op_arity(n.op) == 0 ? 0 : n.op == Op::Not ? m : m + 1);
    }
    return lv[x];
  };

  std::vector<uint32_t> map;
  std::vector<uint32_t> leaves, stack;
  using Item = std::pair<int, uint32_t>;
  rebuild(g, out, map, [&](uint32_t i, Op op, uint32_t a, uint32_t b, uint32_t c) -> uint32_t {
    if (!associative(op)) return out.add(op, a, b, c);
    if (inner[i]) return kNone;  // built as part of its parent
    leaves.clear();
    stack.assign({g.nodes[i].in[1], g.nodes[i].in[0]});
    while (!stack.empty()) {
      uint32_t x = stack.back();
      stack.pop_back();
      if (inner[x]) {
        stack.push_back(g.nodes[x].in[1]);
        stack.push_back(g.nodes[x].in[0]);
      } else {
        leaves.push_back(map[x]);
      }
    }
    if (leaves.size() == 2) return out.add(op, leaves[0], leaves[1]);
    // Huffman-style: always combine the two shallowest signals
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (uint32_t l : leaves) pq.push({level(l), l});
    while (pq.size() > 1) {
      uint32_t x = pq.top().second;
      pq.pop();
      uint32_t y = pq.top().second;
      pq.pop();
      uint32_t z = out.add(op, x, y);
      pq.push({level(z), z});
    }
    return pq.top().second;
  });
  g = std::move(out);
}

void sweep(Graph& g, int max_support) {
  if (max_support < 1) return;
  const size_t n = g.size();
  constexpr int W = 4;  // 256 random patterns per node
  std::vector<uint64_t> sim(n * W);
  uint64_t seed = 0x5eed;
  for (uint32_t i = 0; i < n; i++) {
    const Node& nd = g.nodes[i];
    uint64_t* v = &sim[size_t(i) * W];
    for (int w = 0; w < W; w++) {
      if (nd.op == Op::Const0) v[w] = 0;
      else if (nd.op == Op::Const1) v[w] = ~0ull;
      else if (is_source(nd.op)) v[w] = splitmix(seed);
      else v[w] = eval_word(nd.op, sim[size_t(nd.in[0]) * W + w], sim[size_t(nd.in[1]) * W + w],
                            sim[size_t(nd.in[2]) * W + w]);
    }
  }

  // Structural support, capped at K sources. Stored flat (K slots per node) so
  // this doesn't do one allocation per gate.
  const int K = max_support;
  std::vector<uint32_t> sup(n * size_t(K));
  std::vector<int8_t> len(n, 0);  // -1: depends on more than K sources, not tracked
  std::vector<uint32_t> tmp;
  for (uint32_t i = 2; i < n; i++) {
    const Node& nd = g.nodes[i];
    if (is_source(nd.op)) {
      sup[size_t(i) * K] = i;
      len[i] = 1;
      continue;
    }
    tmp.clear();
    bool too_big = false;
    for (int j = 0; j < op_arity(nd.op) && !too_big; j++) {
      uint32_t f = nd.in[j];
      if (len[f] < 0) too_big = true;
      else tmp.insert(tmp.end(), &sup[size_t(f) * K], &sup[size_t(f) * K] + len[f]);
    }
    if (!too_big) {
      std::sort(tmp.begin(), tmp.end());
      tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
      too_big = (int)tmp.size() > K;
    }
    if (too_big) {
      len[i] = -1;
      continue;
    }
    std::copy(tmp.begin(), tmp.end(), &sup[size_t(i) * K]);
    len[i] = int8_t(tmp.size());
  }

  // exhaustive check of x == y over the union of their supports
  std::vector<uint32_t> stamp(n, 0), local(n, 0), cone, vars, st;
  std::vector<uint64_t> val;
  uint32_t epoch = 0;
  auto prove = [&](uint32_t x, uint32_t y) {
    vars.clear();
    const uint32_t* sx = &sup[size_t(x) * K];
    const uint32_t* sy = &sup[size_t(y) * K];
    std::set_union(sx, sx + len[x], sy, sy + len[y], std::back_inserter(vars));
    if ((int)vars.size() > K) return false;
    size_t words = vars.size() <= 6 ? 1 : size_t(1) << (vars.size() - 6);
    epoch++;
    cone.clear();
    st.assign({x, y});
    while (!st.empty()) {
      uint32_t v = st.back();
      st.pop_back();
      if (v < 2 || stamp[v] == epoch) continue;
      stamp[v] = epoch;
      cone.push_back(v);
      const Node& nd = g.nodes[v];
      for (int j = 0; j < op_arity(nd.op); j++) st.push_back(nd.in[j]);
    }
    std::sort(cone.begin(), cone.end());
    val.assign((cone.size() + 2) * words, 0);
    for (size_t w = 0; w < words; w++) val[1 * words + w] = ~0ull;  // slot 0 = const0, slot 1 = const1
    auto slot = [&](uint32_t v) -> size_t { return v < 2 ? v : local[v]; };
    for (size_t k = 0; k < cone.size(); k++) local[cone[k]] = uint32_t(k + 2);
    for (size_t k = 0; k < cone.size(); k++) {
      uint32_t v = cone[k];
      const Node& nd = g.nodes[v];
      uint64_t* out = &val[(k + 2) * words];
      if (is_source(nd.op)) {
        int var = int(std::lower_bound(vars.begin(), vars.end(), v) - vars.begin());
        for (size_t w = 0; w < words; w++) out[w] = var_word(var, w);
        continue;
      }
      const uint64_t* a = &val[slot(nd.in[0]) * words];
      const uint64_t* b = &val[slot(nd.in[1]) * words];
      const uint64_t* c = &val[slot(nd.in[2]) * words];
      for (size_t w = 0; w < words; w++) out[w] = eval_word(nd.op, a[w], b[w], c[w]);
    }
    const uint64_t* a = &val[slot(x) * words];
    const uint64_t* b = &val[slot(y) * words];
    return std::equal(a, a + words, b);
  };

  // signature hash -> first node seen with it (open addressing, no allocation per node)
  size_t cap = 64;
  while (cap < n * 2) cap <<= 1;
  std::vector<uint64_t> keys(cap);
  std::vector<uint32_t> reps(cap, kNone);
  auto lookup = [&](uint64_t h) -> uint32_t& {
    size_t s = h & (cap - 1);
    while (reps[s] != kNone && keys[s] != h) s = (s + 1) & (cap - 1);
    keys[s] = h;
    return reps[s];
  };

  std::vector<uint32_t> repl(n, kNone);
  for (uint32_t i = 2; i < n; i++) {
    const Node& nd = g.nodes[i];
    if (len[i] < 0) continue;
    const uint64_t* v = &sim[size_t(i) * W];
    bool zero = true, ones = true;
    uint64_t h = 1469598103934665603ull;
    for (int w = 0; w < W; w++) {
      zero &= v[w] == 0;
      ones &= v[w] == ~0ull;
      h = (h ^ v[w]) * 1099511628211ull;
    }
    if (!is_source(nd.op)) {
      if (zero && prove(i, kFalse)) {
        repl[i] = kFalse;
        continue;
      }
      if (ones && prove(i, kTrue)) {
        repl[i] = kTrue;
        continue;
      }
    }
    // inputs are representatives too, so (a&b)|(a&~b) collapses onto a
    uint32_t& rep = lookup(h);
    if (rep == kNone) {
      rep = i;
      continue;
    }
    // one real attempt per node keeps this linear-ish
    if (!is_source(nd.op) && std::equal(v, v + W, &sim[size_t(rep) * W]) && prove(i, rep)) repl[i] = rep;
  }

  Graph out = g.empty_like();
  Builder mk(out, true, true, true, g.size());
  std::vector<uint32_t> map;
  rebuild(g, out, map, [&](uint32_t i, Op op, uint32_t a, uint32_t b, uint32_t c) {
    if (repl[i] != kNone) return map[repl[i]];
    return mk.make(op, a, b, c);
  });
  g = std::move(out);
}

void merge_logs(std::vector<PassLog>& into, const std::vector<PassLog>& from) {
  for (const auto& p : from) {
    auto it = std::find_if(into.begin(), into.end(), [&](const PassLog& q) { return q.name == p.name; });
    if (it == into.end()) {
      into.push_back(p);
    } else {
      it->gates_removed += p.gates_removed;
      it->ms += p.ms;
      it->runs += p.runs;
    }
  }
}

void optimize(Graph& g, const PassOptions& o, std::vector<PassLog>* log) {
  std::vector<PassLog> local;
  auto gates = [&] {
    long c = 0;
    for (const Node& n : g.nodes) c += is_gate(n.op);
    return c;
  };
  auto run = [&](const char* name, auto&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    long before = gates();
    fn();
    if (o.verify_each) verify(g);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    merge_logs(local, {{name, before - gates(), ms, 1}});
  };
  for (int it = 0; it < o.max_iters; it++) {
    Qor before = measure(g);
    run("constprop", [&] { const_prop(g); });
    run("simplify", [&] { simplify(g); });
    run("strash", [&] { strash(g); });
    run("dce", [&] { dce(g); });
    if (o.balance) run("balance", [&] { balance(g); });
    if (o.sweep) run("sweep", [&] { sweep(g, o.sweep_support); });
    run("strash", [&] { strash(g); });
    run("dce", [&] { dce(g); });
    Qor after = measure(g);
    if (after.area >= before.area && after.depth >= before.depth) break;
  }
  if (log) merge_logs(*log, local);
}

}  // namespace lf

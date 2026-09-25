#include "ir.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace lf {

const char* op_name(Op op) {
  switch (op) {
    case Op::Const0: return "const0";
    case Op::Const1: return "const1";
    case Op::Input: return "input";
    case Op::FlopQ: return "flopq";
    case Op::Not: return "not";
    case Op::And: return "and";
    case Op::Or: return "or";
    case Op::Xor: return "xor";
    case Op::Mux: return "mux";
  }
  return "?";
}

int op_arity(Op op) {
  switch (op) {
    case Op::Not: return 1;
    case Op::And:
    case Op::Or:
    case Op::Xor: return 2;
    case Op::Mux: return 3;
    default: return 0;
  }
}

Graph::Graph() {
  nodes.push_back({Op::Const0, {0, 0, 0}});
  nodes.push_back({Op::Const1, {0, 0, 0}});
}

uint32_t Graph::add(Op op, uint32_t a, uint32_t b, uint32_t c) {
  nodes.push_back({op, {a, b, c}});
  return uint32_t(nodes.size() - 1);
}

Graph Graph::empty_like() const {
  Graph g;
  g.module = module;
  g.nets = nets;
  return g;
}

std::vector<int> levels(const Graph& g) {
  std::vector<int> lv(g.size(), 0);
  for (uint32_t i = 0; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    int k = op_arity(n.op);
    if (k == 0) continue;
    int m = 0;
    for (int j = 0; j < k; j++) m = std::max(m, lv[n.in[j]]);
    // inverters are free in most cost models; don't let them inflate depth
    lv[i] = n.op == Op::Not ? m : m + 1;
  }
  return lv;
}

std::vector<uint32_t> fanout_counts(const Graph& g) {
  std::vector<uint32_t> fo(g.size(), 0);
  for (const Node& n : g.nodes)
    for (int j = 0; j < op_arity(n.op); j++) fo[n.in[j]]++;
  for (const auto& o : g.outputs) fo[o.node]++;
  for (const auto& f : g.flops) fo[f.d]++;
  return fo;
}

Qor measure(const Graph& g) {
  Qor q;
  for (const Node& n : g.nodes) {
    switch (n.op) {
      case Op::Not: q.nots++; break;
      case Op::And: q.ands++; break;
      case Op::Or: q.ors++; break;
      case Op::Xor: q.xors++; break;
      case Op::Mux: q.muxes++; break;
      default: break;
    }
  }
  q.gates = q.nots + q.ands + q.ors + q.xors + q.muxes;
  q.area = q.ands + q.ors + 3 * (q.xors + q.muxes);
  q.inputs = g.inputs.size();
  q.outputs = g.outputs.size();
  q.flops = g.flops.size();
  auto lv = levels(g);
  for (const auto& o : g.outputs) q.depth = std::max(q.depth, lv[o.node]);
  for (const auto& f : g.flops) q.depth = std::max(q.depth, lv[f.d]);
  return q;
}

void verify(const Graph& g) {
  auto fail = [&](const std::string& m) { throw std::runtime_error("IR verify (" + g.module + "): " + m); };
  if (g.size() < 2 || g.nodes[0].op != Op::Const0 || g.nodes[1].op != Op::Const1)
    fail("nodes 0/1 must be the constants");
  size_t n_in = 0, n_q = 0;
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    if (n.op == Op::Const0 || n.op == Op::Const1) fail("constant at id " + std::to_string(i));
    if (n.op == Op::Input) n_in++;
    if (n.op == Op::FlopQ) n_q++;
    for (int j = 0; j < op_arity(n.op); j++)
      if (n.in[j] >= i) fail("node " + std::to_string(i) + " reads a later node " + std::to_string(n.in[j]));
  }
  if (n_in != g.inputs.size()) fail("input node count does not match input list");
  if (n_q != g.flops.size()) fail("flopq node count does not match flop list");
  for (const auto& b : g.inputs)
    if (b.node >= g.size() || g.nodes[b.node].op != Op::Input) fail("input " + b.name + " points at a non-input");
  for (const auto& f : g.flops) {
    if (f.q >= g.size() || g.nodes[f.q].op != Op::FlopQ) fail("flop " + f.name + " q is not a flopq node");
    if (f.d >= g.size()) fail("flop " + f.name + " d out of range");
  }
  for (const auto& o : g.outputs)
    if (o.node >= g.size()) fail("output " + o.name + " out of range");
}

std::string dump(const Graph& g) {
  std::ostringstream os;
  os << "module " << g.module << "\n";
  for (const auto& b : g.inputs) os << "  n" << b.node << " = input " << b.name << "[" << b.bit << "]\n";
  for (const auto& f : g.flops) os << "  n" << f.q << " = flop " << f.name << "[" << f.bit << "] d=n" << f.d << "\n";
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    if (is_source(n.op)) continue;
    os << "  n" << i << " = " << op_name(n.op);
    for (int j = 0; j < op_arity(n.op); j++) os << " n" << n.in[j];
    os << "\n";
  }
  for (const auto& o : g.outputs) os << "  " << o.name << "[" << o.bit << "] <- n" << o.node << "\n";
  return os.str();
}

}  // namespace lf

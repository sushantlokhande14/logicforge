#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lf {

// Gate-level IR. Every node is one bit. Mux(s, a, b) means s ? a : b.
// Fanins always have smaller ids than the node itself, so the node vector is
// already in topological order and every pass is a single forward sweep.
enum class Op : uint8_t { Const0, Const1, Input, FlopQ, Not, And, Or, Xor, Mux };

const char* op_name(Op op);
int op_arity(Op op);
inline bool is_source(Op op) { return op <= Op::FlopQ; }
inline bool is_gate(Op op) { return op >= Op::Not; }

struct Node {
  Op op;
  uint32_t in[3];
};

// A named bit that lives on a port or register, e.g. sum[3].
struct BitRef {
  std::string name;
  int bit;
  uint32_t node;
};

struct Flop {
  std::string name;
  int bit;
  std::string clock;
  uint32_t q;  // FlopQ node that the logic reads
  uint32_t d;  // next-state driver
};

enum class NetKind : uint8_t { Input, Output, Reg };

// Declaration info the writer needs to print ports and registers back out.
struct NetDecl {
  std::string name;
  NetKind kind;
  int msb, lsb;
  bool vector;
};

constexpr uint32_t kFalse = 0;
constexpr uint32_t kTrue = 1;

struct Graph {
  std::string module;
  std::vector<NetDecl> nets;
  std::vector<Node> nodes;
  std::vector<BitRef> inputs;
  std::vector<BitRef> outputs;
  std::vector<Flop> flops;

  Graph();
  uint32_t add(Op op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
  size_t size() const { return nodes.size(); }
  const Node& at(uint32_t i) const { return nodes[i]; }
  // Copies name/nets and gives an empty body (just the two constants).
  Graph empty_like() const;
};

struct Qor {
  size_t gates = 0, nots = 0, ands = 0, ors = 0, xors = 0, muxes = 0;
  // AIG-equivalent node count: and/or = 1, xor/mux = 3, inverters are free.
  // A better "how big is this" number than raw gates, since a mux != an and.
  size_t area = 0;
  size_t inputs = 0, outputs = 0, flops = 0;
  int depth = 0;
};

std::vector<int> levels(const Graph& g);
std::vector<uint32_t> fanout_counts(const Graph& g);  // outputs and flop D pins count too
Qor measure(const Graph& g);

// Structural sanity check (topological order, ids in range, sources consistent).
// Throws std::runtime_error with a description; cheap enough to run after every pass.
void verify(const Graph& g);

std::string dump(const Graph& g);

}  // namespace lf

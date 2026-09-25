#pragma once

#include <utility>
#include <vector>

#include "ir.h"

namespace lf {

constexpr uint32_t kNoNode = UINT32_MAX;

inline bool commutative(Op op) { return op == Op::And || op == Op::Or || op == Op::Xor; }

// Node factory used by the rebuilding passes. Which rewrites it applies is
// controlled by three flags, so each pass is just a Builder configuration:
//   fold  - constant folding (x & 0 = 0, mux with constant select, ...)
//   simp  - Boolean identities that don't need constants (x & ~x, ~~x, absorption)
//   hash  - structural hashing: an identical gate that already exists is reused
// Open-addressing table of node ids keyed by (op, fanins). The key is read back
// from the graph itself, so a slot is just 4 bytes and inserting never allocates.
// (std::unordered_map allocated once per gate and was the top malloc user,
// which hurt even more with 16 threads allocating at once.)
class StrashTable {
 public:
  void init(size_t expected) {
    size_t n = 64;
    while (n < expected * 2) n <<= 1;
    slots_.assign(n, kNoNode);
    used_ = 0;
  }

  uint32_t find_or_add(Graph& g, Op op, uint32_t a, uint32_t b, uint32_t c) {
    if (slots_.empty()) init(64);
    size_t mask = slots_.size() - 1;
    for (size_t h = hash(op, a, b, c) & mask;; h = (h + 1) & mask) {
      uint32_t id = slots_[h];
      if (id == kNoNode) {
        id = g.add(op, a, b, c);
        slots_[h] = id;
        if (++used_ * 2 > slots_.size()) grow(g);
        return id;
      }
      const Node& n = g.nodes[id];
      if (n.op == op && n.in[0] == a && n.in[1] == b && n.in[2] == c) return id;
    }
  }

 private:
  static size_t hash(Op op, uint32_t a, uint32_t b, uint32_t c) {
    uint64_t h = uint64_t(op) + 0x9E3779B97F4A7C15ull;
    h = (h ^ a) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ b) * 0x94D049BB133111EBull;
    h = (h ^ c) * 0x9E3779B97F4A7C15ull;
    return size_t(h ^ (h >> 29));
  }

  void grow(const Graph& g) {
    std::vector<uint32_t> old;
    old.swap(slots_);
    slots_.assign(old.size() * 2, kNoNode);
    size_t mask = slots_.size() - 1;
    for (uint32_t id : old) {
      if (id == kNoNode) continue;
      const Node& n = g.nodes[id];
      size_t h = hash(n.op, n.in[0], n.in[1], n.in[2]) & mask;
      while (slots_[h] != kNoNode) h = (h + 1) & mask;
      slots_[h] = id;
    }
  }

  std::vector<uint32_t> slots_;
  size_t used_ = 0;
};

struct Builder {
  Graph& g;
  bool fold, simp, hash;
  StrashTable table;

  Builder(Graph& out, bool f, bool s, bool h, size_t expected = 0) : g(out), fold(f), simp(s), hash(h) {
    if (hash) table.init(expected);
  }

  const Node& at(uint32_t x) const { return g.nodes[x]; }
  bool inv(uint32_t x, uint32_t& inner) const {
    if (at(x).op != Op::Not) return false;
    inner = at(x).in[0];
    return true;
  }
  bool complement(uint32_t x, uint32_t y) const {
    uint32_t i;
    return (inv(x, i) && i == y) || (inv(y, i) && i == x);
  }
  // x is an `op` gate with y as one of its inputs
  bool has_input(uint32_t x, Op op, uint32_t y) const {
    const Node& n = at(x);
    return n.op == op && (n.in[0] == y || n.in[1] == y);
  }

  uint32_t make(Op op, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    if (fold) {
      uint32_t r = fold_const(op, a, b, c);
      if (r != kNoNode) return r;
    }
    if (simp) {
      uint32_t r = simplify(op, a, b, c);
      if (r != kNoNode) return r;
    }
    if (commutative(op) && a > b) std::swap(a, b);
    if (!hash) return g.add(op, a, b, c);
    return table.find_or_add(g, op, a, b, c);
  }

  uint32_t fold_const(Op op, uint32_t a, uint32_t b, uint32_t c) {
    switch (op) {
      case Op::Not:
        if (a == kFalse) return kTrue;
        if (a == kTrue) return kFalse;
        break;
      case Op::And:
        if (a == kFalse || b == kFalse) return kFalse;
        if (a == kTrue) return b;
        if (b == kTrue) return a;
        break;
      case Op::Or:
        if (a == kTrue || b == kTrue) return kTrue;
        if (a == kFalse) return b;
        if (b == kFalse) return a;
        break;
      case Op::Xor:
        if (a == kFalse) return b;
        if (b == kFalse) return a;
        if (a == kTrue) return make(Op::Not, b);
        if (b == kTrue) return make(Op::Not, a);
        break;
      case Op::Mux:  // a ? b : c
        if (a == kTrue) return b;
        if (a == kFalse) return c;
        if (b == kTrue && c == kFalse) return a;
        if (b == kFalse && c == kTrue) return make(Op::Not, a);
        if (c == kFalse) return make(Op::And, a, b);
        if (b == kTrue) return make(Op::Or, a, c);
        if (b == kFalse) return make(Op::And, make(Op::Not, a), c);
        if (c == kTrue) return make(Op::Or, make(Op::Not, a), b);
        break;
      default:
        break;
    }
    return kNoNode;
  }

  uint32_t simplify(Op op, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x, y;
    switch (op) {
      case Op::Not:
        if (inv(a, x)) return x;  // ~~x = x
        break;
      case Op::And:
        if (a == b) return a;
        if (complement(a, b)) return kFalse;
        if (has_input(b, Op::Or, a)) return a;  // a & (a | y) = a
        if (has_input(a, Op::Or, b)) return b;
        if (has_input(b, Op::And, a)) return b;  // a & (a & y) = a & y
        if (has_input(a, Op::And, b)) return a;
        break;
      case Op::Or:
        if (a == b) return a;
        if (complement(a, b)) return kTrue;
        if (has_input(b, Op::And, a)) return a;  // a | (a & y) = a
        if (has_input(a, Op::And, b)) return b;
        if (has_input(b, Op::Or, a)) return b;
        if (has_input(a, Op::Or, b)) return a;
        break;
      case Op::Xor:
        if (a == b) return kFalse;
        if (complement(a, b)) return kTrue;
        if (inv(a, x) && inv(b, y)) return make(Op::Xor, x, y);
        if (inv(a, x)) return make(Op::Not, make(Op::Xor, x, b));
        if (inv(b, y)) return make(Op::Not, make(Op::Xor, a, y));
        break;
      case Op::Mux:
        if (b == c) return b;
        if (inv(a, x)) return make(Op::Mux, x, c, b);
        if (a == b) return make(Op::Or, a, c);   // s ? s : e
        if (a == c) return make(Op::And, a, b);  // s ? t : s
        if (inv(b, x) && x == c) return make(Op::Xor, a, c);               // s ? ~e : e
        if (inv(c, y) && y == b) return make(Op::Not, make(Op::Xor, a, b));  // s ? t : ~t
        break;
      default:
        break;
    }
    return kNoNode;
  }
};

}  // namespace lf

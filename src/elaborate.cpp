#include "elaborate.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace lf {

namespace {

using Bits = std::vector<uint32_t>;
constexpr int64_t kUnset = -1;

enum class Drv : uint8_t { None, Input, Assign, Comb, Ff };

struct Net {
  std::string name;
  Decl::Dir dir;
  int msb = 0, lsb = 0, width = 1;
  bool vector = false;
  int line = 0;
  std::vector<Drv> drv;
  std::vector<int> drv_idx;   // which assign / always block
  std::vector<int> drv_off;   // bit offset inside that assign's lhs
  std::vector<uint32_t> src;  // Input or FlopQ node for input/ff bits
};

// per-block symbolic environment: net index -> bit values (kUnset = not assigned yet)
using Env = std::map<int, std::vector<int64_t>>;

struct Scope {
  int block = -1;  // -1: continuous assign
  bool ff = false;
  Env* env = nullptr;
};

class Elab {
 public:
  explicit Elab(const Module& m) : m_(m) {}

  Graph run() {
    g_.module = m_.name;
    for (const auto& p : m_.params) params_[p.name] = const_eval(*p.value);
    declare();
    for (size_t k = 0; k < m_.assigns.size(); k++) {
      auto tg = targets(*m_.assigns[k].lhs);
      for (size_t j = 0; j < tg.size(); j++) drive(tg[j], Drv::Assign, (int)k, (int)j, m_.assigns[k].line);
    }
    for (size_t b = 0; b < m_.blocks.size(); b++) {
      std::vector<std::pair<int, int>> tg;
      collect_targets(*m_.blocks[b].body, tg);
      for (auto t : tg) drive(t, m_.blocks[b].ff ? Drv::Ff : Drv::Comb, (int)b, 0, m_.blocks[b].line);
    }
    assign_state_.assign(m_.assigns.size(), 0);
    assign_val_.resize(m_.assigns.size());
    block_state_.assign(m_.blocks.size(), 0);
    block_env_.resize(m_.blocks.size());

    // outputs first, then everything else so dead logic also ends up in the graph
    for (int ni : port_nets_) {
      Net& n = nets_[ni];
      if (n.dir != Decl::Out) continue;
      for (int p = 0; p < n.width; p++) g_.outputs.push_back({n.name, index_of(n, p), read(ni, p, n.line, nullptr)});
    }
    for (size_t k = 0; k < m_.assigns.size(); k++) eval_assign((int)k);
    for (size_t b = 0; b < m_.blocks.size(); b++) eval_block((int)b);
    for (auto& f : flop_info_) {
      auto& env = block_env_[f.block];
      g_.flops[f.flop].d = (uint32_t)env[f.net][f.pos];
    }
    return std::move(g_);
  }

 private:
  [[noreturn]] void fail(int line, const std::string& msg) {
    throw std::runtime_error(m_.file + ":" + std::to_string(line) + ": " + msg);
  }

  // ---- constants and params ----------------------------------------------

  int64_t const_eval(const Expr& e) {
    switch (e.kind) {
      case Expr::Number: {
        int64_t v = 0;
        for (size_t i = 0; i < e.bits.size() && i < 63; i++)
          if (e.bits[i]) v |= int64_t(1) << i;
        return v;
      }
      case Expr::Ident: {
        auto it = params_.find(e.name);
        if (it == params_.end()) fail(e.line, "'" + e.name + "' is not a constant");
        return it->second;
      }
      case Expr::Unary:
        if (e.op == "-") return -const_eval(*e.args[0]);
        break;
      case Expr::Binary: {
        int64_t a = const_eval(*e.args[0]), b = const_eval(*e.args[1]);
        if (e.op == "+") return a + b;
        if (e.op == "-") return a - b;
        if (e.op == "*") return a * b;
        if (e.op == "<<") return a << b;
        if (e.op == ">>") return a >> b;
        break;
      }
      default:
        break;
    }
    fail(e.line, "expression is not a supported constant");
  }

  // ---- nets ---------------------------------------------------------------

  void declare() {
    for (const auto& d : m_.decls) {
      auto it = net_idx_.find(d.name);
      if (it != net_idx_.end()) {
        // non-ANSI style: "output [3:0] y;" followed by "reg [3:0] y;" is fine
        if (d.dir == Decl::Internal) continue;
        Net& n = nets_[it->second];
        if (n.dir != Decl::Internal) fail(d.line, "'" + d.name + "' declared twice");
        n.dir = d.dir;
        continue;
      }
      Net n;
      n.name = d.name;
      n.dir = d.dir;
      n.line = d.line;
      if (d.msb) {
        n.msb = (int)const_eval(*d.msb);
        n.lsb = (int)const_eval(*d.lsb);
        n.vector = true;
      }
      n.width = std::abs(n.msb - n.lsb) + 1;
      n.drv.assign(n.width, Drv::None);
      n.drv_idx.assign(n.width, -1);
      n.drv_off.assign(n.width, -1);
      n.src.assign(n.width, 0);
      net_idx_[n.name] = (int)nets_.size();
      nets_.push_back(std::move(n));
    }
    // port order: header order if given, declaration order otherwise
    std::vector<std::string> order = m_.port_order;
    if (order.empty())
      for (const auto& n : nets_)
        if (n.dir != Decl::Internal) order.push_back(n.name);
    for (const auto& name : order) {
      auto it = net_idx_.find(name);
      if (it == net_idx_.end() || nets_[it->second].dir == Decl::Internal)
        fail(m_.line, "port '" + name + "' has no input/output declaration");
      port_nets_.push_back(it->second);
      Net& n = nets_[it->second];
      g_.nets.push_back({n.name, n.dir == Decl::In ? NetKind::Input : NetKind::Output, n.msb, n.lsb, n.vector});
      if (n.dir == Decl::In)
        for (int p = 0; p < n.width; p++) {
          n.drv[p] = Drv::Input;
          n.src[p] = g_.add(Op::Input);
          g_.inputs.push_back({n.name, index_of(n, p), n.src[p]});
        }
    }
  }

  int index_of(const Net& n, int pos) const { return n.msb >= n.lsb ? n.lsb + pos : n.lsb - pos; }

  int pos_of(const Net& n, int64_t index, int line) {
    int64_t p = n.msb >= n.lsb ? index - n.lsb : n.lsb - index;
    if (p < 0 || p >= n.width) fail(line, "index " + std::to_string(index) + " out of range for '" + n.name + "'");
    return (int)p;
  }

  int net_of(const std::string& name, int line) {
    auto it = net_idx_.find(name);
    if (it == net_idx_.end()) fail(line, "undeclared identifier '" + name + "'");
    return it->second;
  }

  // (net, pos) list for an lvalue, LSB first
  std::vector<std::pair<int, int>> targets(const Expr& e) {
    std::vector<std::pair<int, int>> out;
    if (e.kind == Expr::Concat) {
      for (auto it = e.args.rbegin(); it != e.args.rend(); ++it) {
        auto sub = targets(**it);
        out.insert(out.end(), sub.begin(), sub.end());
      }
      return out;
    }
    int ni = net_of(e.name, e.line);
    const Net& n = nets_[ni];
    if (n.dir == Decl::In) fail(e.line, "cannot assign to input '" + n.name + "'");
    if (e.kind == Expr::Ident) {
      for (int p = 0; p < n.width; p++) out.push_back({ni, p});
    } else if (e.kind == Expr::Index) {
      out.push_back({ni, pos_of(n, const_eval(*e.args[0]), e.line)});
    } else if (e.kind == Expr::Slice) {
      int hi = pos_of(n, const_eval(*e.args[0]), e.line), lo = pos_of(n, const_eval(*e.args[1]), e.line);
      if (hi < lo) fail(e.line, "reversed part-select on '" + n.name + "'");
      for (int p = lo; p <= hi; p++) out.push_back({ni, p});
    } else {
      fail(e.line, "not an assignable expression");
    }
    return out;
  }

  void collect_targets(const Stmt& s, std::vector<std::pair<int, int>>& out) {
    if (s.kind == Stmt::Assign) {
      auto t = targets(*s.lhs);
      out.insert(out.end(), t.begin(), t.end());
    }
    for (const auto& c : s.body) collect_targets(*c, out);
    for (const auto& c : s.orelse) collect_targets(*c, out);
  }

  void drive(std::pair<int, int> t, Drv kind, int idx, int off, int line) {
    Net& n = nets_[t.first];
    int p = t.second;
    if (n.drv[p] != Drv::None) {
      bool same_block = (kind == Drv::Comb || kind == Drv::Ff) && n.drv[p] == kind && n.drv_idx[p] == idx;
      if (same_block) return;
      fail(line, "'" + n.name + "[" + std::to_string(index_of(n, p)) + "]' has more than one driver");
    }
    n.drv[p] = kind;
    n.drv_idx[p] = idx;
    n.drv_off[p] = off;
    if (kind == Drv::Ff) {
      n.src[p] = g_.add(Op::FlopQ);
      flop_info_.push_back({(int)g_.flops.size(), idx, t.first, p});
      g_.flops.push_back({n.name, index_of(n, p), m_.blocks[idx].clock, n.src[p], 0});
      bool seen = false;
      for (const auto& d : g_.nets) seen |= d.kind == NetKind::Reg && d.name == n.name;
      if (!seen) g_.nets.push_back({n.name, NetKind::Reg, n.msb, n.lsb, n.vector});
    }
  }

  // ---- evaluation ---------------------------------------------------------

  uint32_t read(int ni, int p, int line, Scope* sc) {
    Net& n = nets_[ni];
    if (sc && sc->block >= 0 && !sc->ff && n.drv[p] == Drv::Comb && n.drv_idx[p] == sc->block) {
      auto it = sc->env->find(ni);
      if (it == sc->env->end() || it->second[p] == kUnset)
        fail(line, "'" + n.name + "' is read before it is assigned in always_comb (latch or loop)");
      return (uint32_t)it->second[p];
    }
    switch (n.drv[p]) {
      case Drv::Input:
      case Drv::Ff:
        return n.src[p];
      case Drv::Assign:
        return eval_assign(n.drv_idx[p])[n.drv_off[p]];
      case Drv::Comb: {
        Env& env = eval_block(n.drv_idx[p]);
        return (uint32_t)env[ni][p];
      }
      case Drv::None:
        break;
    }
    fail(line, "'" + n.name + "[" + std::to_string(index_of(n, p)) + "]' is never driven");
  }

  const Bits& eval_assign(int k) {
    if (assign_state_[k] == 2) return assign_val_[k];
    const ContAssign& a = m_.assigns[k];
    if (assign_state_[k] == 1) fail(a.line, "combinational loop through this assign");
    assign_state_[k] = 1;
    int w = (int)targets(*a.lhs).size();
    assign_val_[k] = fit(expr(*a.rhs, w, nullptr), w);
    assign_state_[k] = 2;
    return assign_val_[k];
  }

  Env& eval_block(int b) {
    if (block_state_[b] == 2) return block_env_[b];
    const Always& blk = m_.blocks[b];
    if (block_state_[b] == 1) fail(blk.line, "combinational loop through this always block");
    block_state_[b] = 1;
    Env env;
    std::vector<std::pair<int, int>> tg;
    collect_targets(*blk.body, tg);
    for (auto [ni, p] : tg) {
      auto& v = env[ni];
      if (v.empty()) v.assign(nets_[ni].width, kUnset);
      // a flop that isn't assigned on some path holds its value
      if (blk.ff) v[p] = nets_[ni].src[p];
    }
    Scope sc{b, blk.ff, &env};
    exec(*blk.body, sc);
    for (auto [ni, p] : tg)
      if (env[ni][p] == kUnset)
        fail(blk.line, "'" + nets_[ni].name + "' is not assigned on every path of always_comb (latch)");
    block_env_[b] = std::move(env);
    block_state_[b] = 2;
    return block_env_[b];
  }

  void exec(const Stmt& s, Scope& sc) {
    switch (s.kind) {
      case Stmt::Block:
        for (const auto& c : s.body) exec(*c, sc);
        return;
      case Stmt::Assign: {
        if (sc.ff && !s.nonblocking) fail(s.line, "use '<=' inside always_ff");
        if (!sc.ff && s.nonblocking) fail(s.line, "use '=' inside always_comb");
        auto tg = targets(*s.lhs);
        Bits v = fit(expr(*s.rhs, (int)tg.size(), &sc), (int)tg.size());
        for (size_t j = 0; j < tg.size(); j++) (*sc.env)[tg[j].first][tg[j].second] = v[j];
        return;
      }
      case Stmt::If: {
        uint32_t c = reduce(Op::Or, expr(*s.cond, 0, &sc));
        Env before = *sc.env;
        for (const auto& t : s.body) exec(*t, sc);
        Env then_env = std::move(*sc.env);
        *sc.env = before;
        for (const auto& e : s.orelse) exec(*e, sc);
        Env& else_env = *sc.env;
        for (auto& [ni, tv] : then_env) {
          auto& ev = else_env[ni];
          for (size_t p = 0; p < tv.size(); p++) {
            if (tv[p] == ev[p]) continue;
            if (tv[p] == kUnset || ev[p] == kUnset) ev[p] = kUnset;
            else ev[p] = g_.add(Op::Mux, c, (uint32_t)tv[p], (uint32_t)ev[p]);
          }
        }
        return;
      }
    }
  }

  // self-determined width, Verilog-style (simplified: everything is unsigned)
  int width(const Expr& e) {
    switch (e.kind) {
      case Expr::Ident:
        if (params_.count(e.name)) return 32;
        return nets_[net_of(e.name, e.line)].width;
      case Expr::Number: return (int)e.bits.size();
      case Expr::Index: return 1;
      case Expr::Slice: return std::abs((int)(const_eval(*e.args[0]) - const_eval(*e.args[1]))) + 1;
      case Expr::Unary:
        return (e.op == "~" || e.op == "-") ? width(*e.args[0]) : 1;
      case Expr::Binary: {
        const std::string& o = e.op;
        if (o == "&" || o == "|" || o == "^" || o == "~^" || o == "+" || o == "-" || o == "*")
          return std::max(width(*e.args[0]), width(*e.args[1]));
        if (o == "<<" || o == ">>") return width(*e.args[0]);
        return 1;
      }
      case Expr::Ternary: return std::max(width(*e.args[1]), width(*e.args[2]));
      case Expr::Concat: {
        int w = 0;
        for (const auto& a : e.args) w += width(*a);
        return w;
      }
      case Expr::Repl: return (int)const_eval(*e.args[0]) * width(*e.args[1]);
    }
    return 1;
  }

  static Bits fit(Bits v, int w) {
    v.resize(w, kFalse);
    return v;
  }

  uint32_t reduce(Op op, const Bits& v) {
    if (v.empty()) return kFalse;
    uint32_t r = v[0];
    for (size_t i = 1; i < v.size(); i++) r = g_.add(op, r, v[i]);  // a chain on purpose; balance() fixes depth
    return r;
  }

  Bits add(const Bits& a, const Bits& b, uint32_t carry) {
    Bits s(a.size());
    for (size_t i = 0; i < a.size(); i++) {
      uint32_t x = g_.add(Op::Xor, a[i], b[i]);
      s[i] = g_.add(Op::Xor, x, carry);
      carry = g_.add(Op::Or, g_.add(Op::And, a[i], b[i]), g_.add(Op::And, carry, x));
    }
    return s;
  }

  // carry out of a + ~b + 1 is 1 exactly when a >= b (unsigned)
  uint32_t less_than(const Bits& a, const Bits& b) {
    uint32_t carry = kTrue;
    for (size_t i = 0; i < a.size(); i++) {
      uint32_t nb = g_.add(Op::Not, b[i]);
      uint32_t x = g_.add(Op::Xor, a[i], nb);
      carry = g_.add(Op::Or, g_.add(Op::And, a[i], nb), g_.add(Op::And, carry, x));
    }
    return g_.add(Op::Not, carry);
  }

  Bits invert(const Bits& a) {
    Bits r(a.size());
    for (size_t i = 0; i < a.size(); i++) r[i] = g_.add(Op::Not, a[i]);
    return r;
  }

  Bits expr(const Expr& e, int ctx, Scope* sc) {
    int w = std::max(ctx, width(e));
    switch (e.kind) {
      case Expr::Number: {
        Bits r(w, kFalse);
        for (size_t i = 0; i < e.bits.size() && i < (size_t)w; i++) r[i] = e.bits[i] ? kTrue : kFalse;
        return r;
      }
      case Expr::Ident: {
        auto pit = params_.find(e.name);
        if (pit != params_.end()) {
          Bits r(w, kFalse);
          for (int i = 0; i < w && i < 63; i++) r[i] = (pit->second >> i) & 1 ? kTrue : kFalse;
          return r;
        }
        int ni = net_of(e.name, e.line);
        Bits r;
        for (int p = 0; p < nets_[ni].width; p++) r.push_back(read(ni, p, e.line, sc));
        return fit(r, w);
      }
      case Expr::Index:
      case Expr::Slice: {
        int ni = net_of(e.name, e.line);
        const Net& n = nets_[ni];
        int hi = pos_of(n, const_eval(*e.args[0]), e.line);
        int lo = e.kind == Expr::Slice ? pos_of(n, const_eval(*e.args[1]), e.line) : hi;
        if (hi < lo) fail(e.line, "reversed part-select on '" + n.name + "'");
        Bits r;
        for (int p = lo; p <= hi; p++) r.push_back(read(ni, p, e.line, sc));
        return fit(r, w);
      }
      case Expr::Unary: {
        const std::string& o = e.op;
        if (o == "~") return invert(expr(*e.args[0], w, sc));
        if (o == "-") {
          Bits zero(w, kFalse);
          return add(zero, invert(expr(*e.args[0], w, sc)), kTrue);
        }
        Bits a = expr(*e.args[0], 0, sc);
        uint32_t r;
        if (o == "&" || o == "~&") r = reduce(Op::And, a);
        else if (o == "|" || o == "~|" || o == "!") r = reduce(Op::Or, a);
        else r = reduce(Op::Xor, a);
        if (o[0] == '~' || o == "!") r = g_.add(Op::Not, r);
        return fit({r}, w);
      }
      case Expr::Binary: {
        const std::string& o = e.op;
        if (o == "&&" || o == "||") {
          uint32_t a = reduce(Op::Or, expr(*e.args[0], 0, sc));
          uint32_t b = reduce(Op::Or, expr(*e.args[1], 0, sc));
          return fit({g_.add(o == "&&" ? Op::And : Op::Or, a, b)}, w);
        }
        if (o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=") {
          int ow = std::max(width(*e.args[0]), width(*e.args[1]));
          Bits a = expr(*e.args[0], ow, sc), b = expr(*e.args[1], ow, sc);
          uint32_t r;
          if (o == "==" || o == "!=") {
            Bits same(ow);
            for (int i = 0; i < ow; i++) same[i] = g_.add(Op::Not, g_.add(Op::Xor, a[i], b[i]));
            r = reduce(Op::And, same);
            if (o == "!=") r = g_.add(Op::Not, r);
          } else if (o == "<") r = less_than(a, b);
          else if (o == ">") r = less_than(b, a);
          else if (o == "<=") r = g_.add(Op::Not, less_than(b, a));
          else r = g_.add(Op::Not, less_than(a, b));
          return fit({r}, w);
        }
        if (o == "<<" || o == ">>") {
          int64_t sh = const_eval(*e.args[1]);
          Bits a = expr(*e.args[0], w, sc), r(w, kFalse);
          for (int i = 0; i < w; i++) {
            int64_t from = o == "<<" ? i - sh : i + sh;
            if (from >= 0 && from < w) r[i] = a[from];
          }
          return r;
        }
        Bits a = expr(*e.args[0], w, sc), b = expr(*e.args[1], w, sc);
        if (o == "+") return add(a, b, kFalse);
        if (o == "-") return add(a, invert(b), kTrue);
        if (o == "*") {
          Bits acc(w, kFalse);
          for (int i = 0; i < w; i++) {
            Bits part(w, kFalse);
            for (int j = 0; i + j < w; j++) part[i + j] = g_.add(Op::And, b[i], a[j]);
            acc = add(acc, part, kFalse);
          }
          return acc;
        }
        Op op = o == "&" ? Op::And : o == "|" ? Op::Or : Op::Xor;
        Bits r(w);
        for (int i = 0; i < w; i++) {
          r[i] = g_.add(op, a[i], b[i]);
          if (o == "~^") r[i] = g_.add(Op::Not, r[i]);
        }
        return r;
      }
      case Expr::Ternary: {
        uint32_t c = reduce(Op::Or, expr(*e.args[0], 0, sc));
        Bits t = expr(*e.args[1], w, sc), f = expr(*e.args[2], w, sc);
        Bits r(w);
        for (int i = 0; i < w; i++) r[i] = g_.add(Op::Mux, c, t[i], f[i]);
        return r;
      }
      case Expr::Concat: {
        Bits r;
        for (auto it = e.args.rbegin(); it != e.args.rend(); ++it) {
          Bits part = expr(**it, 0, sc);
          r.insert(r.end(), part.begin(), part.end());
        }
        return fit(r, w);
      }
      case Expr::Repl: {
        int64_t n = const_eval(*e.args[0]);
        if (n <= 0) fail(e.line, "replication count must be positive");
        Bits inner = expr(*e.args[1], 0, sc), r;
        for (int64_t i = 0; i < n; i++) r.insert(r.end(), inner.begin(), inner.end());
        return fit(r, w);
      }
    }
    fail(e.line, "unhandled expression");
  }

  struct FlopRef {
    int flop, block, net, pos;
  };

  const Module& m_;
  Graph g_;
  std::unordered_map<std::string, int64_t> params_;
  std::vector<Net> nets_;
  std::unordered_map<std::string, int> net_idx_;
  std::vector<int> port_nets_;
  std::vector<FlopRef> flop_info_;
  std::vector<int> assign_state_, block_state_;
  std::vector<Bits> assign_val_;
  std::vector<Env> block_env_;
};

}  // namespace

Graph elaborate(const Module& m) { return Elab(m).run(); }

}  // namespace lf

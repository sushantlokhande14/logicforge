// Lexer + recursive-descent parser for a synthesizable Verilog/SystemVerilog subset.
// See docs/language.md for what is and isn't accepted.

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "ast.h"

namespace lf {

ExprP clone(const Expr& e) {
  auto c = std::make_unique<Expr>();
  c->kind = e.kind;
  c->line = e.line;
  c->name = e.name;
  c->op = e.op;
  c->bits = e.bits;
  for (const auto& a : e.args) c->args.push_back(clone(*a));
  return c;
}

namespace {

struct Tok {
  enum Kind { Id, Num, Sym, End } kind;
  std::string text;
  int line;
  std::vector<bool> bits;  // Num only
};

[[noreturn]] void error(const std::string& file, int line, const std::string& msg) {
  throw std::runtime_error(file + ":" + std::to_string(line) + ": " + msg);
}

std::vector<bool> to_bits(uint64_t v, int width) {
  std::vector<bool> b(width);
  for (int i = 0; i < width && i < 64; i++) b[i] = (v >> i) & 1;
  return b;
}

class Lexer {
 public:
  Lexer(const std::string& src, const std::string& file) : s_(src), file_(file) {}

  std::vector<Tok> run() {
    std::vector<Tok> out;
    for (;;) {
      skip_space();
      if (p_ >= s_.size()) break;
      char c = s_[p_];
      if (c == '`') {  // compiler directive: ignore the rest of the line
        while (p_ < s_.size() && s_[p_] != '\n') p_++;
        continue;
      }
      if (std::isalpha((unsigned char)c) || c == '_' || c == '$') {
        size_t b = p_;
        while (p_ < s_.size() && (std::isalnum((unsigned char)s_[p_]) || s_[p_] == '_' || s_[p_] == '$')) p_++;
        out.push_back({Tok::Id, s_.substr(b, p_ - b), line_, {}});
      } else if (std::isdigit((unsigned char)c) || c == '\'') {
        out.push_back(number());
      } else {
        out.push_back(symbol());
      }
    }
    out.push_back({Tok::End, "<eof>", line_, {}});
    return out;
  }

 private:
  void skip_space() {
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == '\n') { line_++; p_++; }
      else if (std::isspace((unsigned char)c)) p_++;
      else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
        while (p_ < s_.size() && s_[p_] != '\n') p_++;
      } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
        p_ += 2;
        while (p_ + 1 < s_.size() && !(s_[p_] == '*' && s_[p_ + 1] == '/')) {
          if (s_[p_] == '\n') line_++;
          p_++;
        }
        p_ += 2;
      } else break;
    }
  }

  uint64_t decimal(const std::string& d) {
    uint64_t v = 0;
    for (char c : d) {
      if (c == '_') continue;
      if (!std::isdigit((unsigned char)c)) error(file_, line_, "bad decimal digit '" + std::string(1, c) + "'");
      if (v > (UINT64_MAX - 9) / 10) error(file_, line_, "decimal constant too large");
      v = v * 10 + (c - '0');
    }
    return v;
  }

  Tok number() {
    size_t b = p_;
    while (p_ < s_.size() && (std::isdigit((unsigned char)s_[p_]) || s_[p_] == '_')) p_++;
    std::string size_txt = s_.substr(b, p_ - b);
    if (p_ >= s_.size() || s_[p_] != '\'') {
      uint64_t v = decimal(size_txt);
      int w = 32;
      while (w < 64 && (v >> w)) w++;
      return {Tok::Num, size_txt, line_, to_bits(v, w)};
    }
    p_++;  // '
    if (p_ < s_.size() && (s_[p_] == 's' || s_[p_] == 'S')) p_++;
    if (p_ >= s_.size()) error(file_, line_, "truncated number");
    char base = (char)std::tolower((unsigned char)s_[p_++]);
    size_t db = p_;
    while (p_ < s_.size() && (std::isalnum((unsigned char)s_[p_]) || s_[p_] == '_')) p_++;
    std::string digits = s_.substr(db, p_ - db);
    int width = size_txt.empty() ? 32 : (int)decimal(size_txt);
    if (width <= 0 || width > 4096) error(file_, line_, "unreasonable constant width");

    std::vector<bool> bits;
    if (base == 'd') {
      bits = to_bits(decimal(digits), 64);
    } else {
      int per = base == 'b' ? 1 : base == 'o' ? 3 : base == 'h' ? 4 : 0;
      if (!per) error(file_, line_, std::string("unknown base '") + base + "'");
      for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        char c = (char)std::tolower((unsigned char)*it);
        if (c == '_') continue;
        if (c == 'x' || c == 'z' || c == '?') error(file_, line_, "x/z literals are not supported");
        int v = std::isdigit((unsigned char)c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 99;
        if (v >= (1 << per)) error(file_, line_, "digit '" + std::string(1, c) + "' out of range for base");
        for (int k = 0; k < per; k++) bits.push_back((v >> k) & 1);
      }
    }
    bits.resize(width, false);
    return {Tok::Num, size_txt + "'" + base + digits, line_, bits};
  }

  Tok symbol() {
    static const char* two[] = {"~^", "^~", "~&", "~|", "&&", "||", "==", "!=", "<=", ">=", "<<", ">>"};
    for (const char* t : two)
      if (s_.compare(p_, 2, t) == 0) {
        p_ += 2;
        return {Tok::Sym, t, line_, {}};
      }
    char c = s_[p_];
    if (std::string("()[]{},;:?=<>@#.~!&|^+-*").find(c) == std::string::npos)
      error(file_, line_, std::string("unexpected character '") + c + "'");
    p_++;
    return {Tok::Sym, std::string(1, c), line_, {}};
  }

  const std::string& s_;
  std::string file_;
  size_t p_ = 0;
  int line_ = 1;
};

class Parser {
 public:
  Parser(std::vector<Tok> toks, std::string file) : t_(std::move(toks)), file_(std::move(file)) {}

  std::vector<Module> run() {
    std::vector<Module> mods;
    while (!at_end()) {
      if (peek("module")) mods.push_back(module());
      else fail("expected 'module', got '" + cur().text + "'");
    }
    return mods;
  }

 private:
  const Tok& cur() const { return t_[i_]; }
  bool at_end() const { return cur().kind == Tok::End; }
  bool peek(const char* s) const { return cur().kind != Tok::Num && cur().text == s; }
  bool accept(const char* s) {
    if (!peek(s)) return false;
    i_++;
    return true;
  }
  void expect(const char* s) {
    if (!accept(s)) fail(std::string("expected '") + s + "', got '" + cur().text + "'");
  }
  std::string ident() {
    if (cur().kind != Tok::Id) fail("expected identifier, got '" + cur().text + "'");
    return t_[i_++].text;
  }
  [[noreturn]] void fail(const std::string& m) { error(file_, cur().line, m); }

  ExprP node(Expr::Kind k) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = cur().line;
    return e;
  }

  // ---- declarations -------------------------------------------------------

  void skip_type(bool* is_range_next = nullptr) {
    while (accept("wire") || accept("logic") || accept("reg") || accept("bit") || accept("signed") ||
           accept("unsigned") || accept("int") || accept("integer")) {
    }
    if (is_range_next) *is_range_next = peek("[");
  }

  void range(ExprP& msb, ExprP& lsb) {
    if (!accept("[")) return;
    msb = expr();
    expect(":");
    lsb = expr();
    expect("]");
  }

  void param_list(Module& m) {  // after 'parameter' or 'localparam'
    skip_type();
    ExprP dm, dl;
    range(dm, dl);  // parameter width is ignored, values are just integers here
    do {
      Param p;
      p.line = cur().line;
      p.name = ident();
      expect("=");
      p.value = expr();
      m.params.push_back(std::move(p));
    } while (accept(",") && !peek("parameter") && !peek("localparam"));
  }

  Module module() {
    Module m;
    m.file = file_;
    m.line = cur().line;
    expect("module");
    m.name = ident();
    if (accept("#")) {
      expect("(");
      while (!accept(")")) {
        if (!accept("parameter")) accept("localparam");
        param_list(m);
      }
    }
    if (accept("(")) {
      if (peek("input") || peek("output") || peek("inout")) ansi_ports(m);
      else if (!peek(")")) {
        do m.port_order.push_back(ident());
        while (accept(","));
      }
      expect(")");
    }
    expect(";");
    while (!accept("endmodule")) {
      if (at_end()) fail("missing endmodule for " + m.name);
      item(m);
    }
    return m;
  }

  void ansi_ports(Module& m) {
    Decl::Dir dir = Decl::In;
    ExprP msb, lsb;
    do {
      if (peek("input") || peek("output") || peek("inout")) {
        if (peek("inout")) fail("inout ports are not supported");
        dir = accept("input") ? Decl::In : (expect("output"), Decl::Out);
        skip_type();
        msb.reset();
        lsb.reset();
        range(msb, lsb);
      }
      Decl d;
      d.dir = dir;
      d.line = cur().line;
      d.name = ident();
      if (msb) {
        d.msb = clone(*msb);
        d.lsb = clone(*lsb);
      }
      m.port_order.push_back(d.name);
      m.decls.push_back(std::move(d));
    } while (accept(","));
  }

  void net_decl(Module& m, Decl::Dir dir) {
    skip_type();
    ExprP msb, lsb;
    range(msb, lsb);
    do {
      Decl d;
      d.dir = dir;
      d.line = cur().line;
      d.name = ident();
      if (msb) {
        d.msb = clone(*msb);
        d.lsb = clone(*lsb);
      }
      if (accept("=")) {  // wire x = expr;
        ContAssign a;
        a.line = d.line;
        a.lhs = node(Expr::Ident);
        a.lhs->name = d.name;
        a.rhs = expr();
        m.assigns.push_back(std::move(a));
      }
      m.decls.push_back(std::move(d));
    } while (accept(","));
    expect(";");
  }

  void item(Module& m) {
    int line = cur().line;
    if (accept("input")) return net_decl(m, Decl::In);
    if (accept("output")) return net_decl(m, Decl::Out);
    if (peek("wire") || peek("logic") || peek("reg") || peek("bit")) return net_decl(m, Decl::Internal);
    if (accept("parameter") || accept("localparam")) {
      param_list(m);
      expect(";");
      return;
    }
    if (accept("assign")) {
      do {
        ContAssign a;
        a.line = line;
        a.lhs = lvalue();
        expect("=");
        a.rhs = expr();
        m.assigns.push_back(std::move(a));
      } while (accept(","));
      expect(";");
      return;
    }
    if (accept("always_comb")) return always(m, false, "", line);
    if (accept("always_ff") || accept("always")) {
      expect("@");
      if (accept("*")) return always(m, false, "", line);
      expect("(");
      if (accept("*")) {
        expect(")");
        return always(m, false, "", line);
      }
      if (!accept("posedge")) fail("only @(posedge clk) or @(*) sensitivity is supported");
      std::string clk = ident();
      if (accept("or") || accept(",")) fail("asynchronous resets are not supported; use a synchronous reset");
      expect(")");
      return always(m, true, clk, line);
    }
    if (peek("initial") || peek("generate") || peek("genvar") || peek("function") || peek("task") ||
        peek("always_latch"))
      fail("'" + cur().text + "' is not supported");
    if (cur().kind == Tok::Id && t_[i_ + 1].kind == Tok::Id)
      fail("module instances are not supported (flatten the design first)");
    fail("unexpected '" + cur().text + "' in module body");
  }

  void always(Module& m, bool ff, const std::string& clk, int line) {
    Always a;
    a.ff = ff;
    a.clock = clk;
    a.line = line;
    a.body = stmt();
    m.blocks.push_back(std::move(a));
  }

  // ---- statements ---------------------------------------------------------

  StmtP stmt() {
    auto s = std::make_unique<Stmt>();
    s->line = cur().line;
    if (accept("begin")) {
      s->kind = Stmt::Block;
      if (accept(":")) ident();
      while (!accept("end")) {
        if (at_end()) fail("missing 'end'");
        s->body.push_back(stmt());
      }
      if (accept(":")) ident();
      return s;
    }
    if (accept("if")) {
      s->kind = Stmt::If;
      expect("(");
      s->cond = expr();
      expect(")");
      s->body.push_back(stmt());
      if (accept("else")) s->orelse.push_back(stmt());
      return s;
    }
    if (accept("unique") || accept("priority") || peek("case")) {
      expect("case");
      return case_stmt(s->line);
    }
    if (peek("casez") || peek("casex")) fail("casez/casex are not supported");
    if (accept(";")) {
      s->kind = Stmt::Block;
      return s;
    }
    s->kind = Stmt::Assign;
    s->lhs = lvalue();
    if (accept("<=")) s->nonblocking = true;
    else expect("=");
    s->rhs = expr();
    expect(";");
    return s;
  }

  // case is lowered straight into an if/else chain: if (sel==a || sel==b) ... else ...
  StmtP case_stmt(int line) {
    expect("(");
    ExprP sel = expr();
    expect(")");
    struct Item {
      ExprP cond;
      StmtP body;
    };
    std::vector<Item> items;
    StmtP deflt;
    while (!accept("endcase")) {
      if (at_end()) fail("missing endcase");
      if (accept("default")) {
        accept(":");
        deflt = stmt();
        continue;
      }
      ExprP cond;
      do {
        auto eq = node(Expr::Binary);
        eq->op = "==";
        eq->args.push_back(clone(*sel));
        eq->args.push_back(expr());
        if (!cond) cond = std::move(eq);
        else {
          auto o = node(Expr::Binary);
          o->op = "||";
          o->args.push_back(std::move(cond));
          o->args.push_back(std::move(eq));
          cond = std::move(o);
        }
      } while (accept(","));
      expect(":");
      items.push_back({std::move(cond), stmt()});
    }
    StmtP tail = std::move(deflt);
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      auto s = std::make_unique<Stmt>();
      s->kind = Stmt::If;
      s->line = line;
      s->cond = std::move(it->cond);
      s->body.push_back(std::move(it->body));
      if (tail) s->orelse.push_back(std::move(tail));
      tail = std::move(s);
    }
    if (!tail) {
      tail = std::make_unique<Stmt>();
      tail->kind = Stmt::Block;
      tail->line = line;
    }
    return tail;
  }

  ExprP lvalue() {
    if (accept("{")) {
      auto e = node(Expr::Concat);
      do e->args.push_back(lvalue());
      while (accept(","));
      expect("}");
      return e;
    }
    return ident_ref();
  }

  ExprP ident_ref() {
    auto e = node(Expr::Ident);
    e->name = ident();
    if (accept("[")) {
      e->args.push_back(expr());
      if (accept(":")) {
        e->kind = Expr::Slice;
        e->args.push_back(expr());
      } else {
        e->kind = Expr::Index;
      }
      expect("]");
    }
    return e;
  }

  // ---- expressions (lowest to highest precedence) ------------------------

  ExprP expr() {
    ExprP c = binary(0);
    if (!accept("?")) return c;
    auto e = node(Expr::Ternary);
    e->args.push_back(std::move(c));
    e->args.push_back(expr());
    expect(":");
    e->args.push_back(expr());
    return e;
  }

  // binding strength of a binary operator, -1 if the token isn't one
  static int prec(const Tok& t) {
    if (t.kind != Tok::Sym) return -1;
    const std::string& s = t.text;
    switch (s[0]) {
      case '|': return s == "||" ? 0 : s == "|" ? 2 : -1;
      case '&': return s == "&&" ? 1 : s == "&" ? 4 : -1;
      case '^': return 3;
      case '~': return s == "~^" ? 3 : -1;
      case '=':
      case '!': return s.size() == 2 && s[1] == '=' ? 5 : -1;
      case '<':
      case '>': return s.size() == 2 && s[1] == s[0] ? 7 : 6;
      case '+':
      case '-': return 8;
      case '*': return 9;
    }
    return -1;
  }

  // precedence climbing: one table lookup per operator instead of one call per level
  ExprP binary(int min_prec) {
    ExprP lhs = unary();
    for (;;) {
      int p = prec(cur());
      if (p < min_prec) return lhs;
      auto e = node(Expr::Binary);
      e->op = t_[i_++].text;
      if (e->op == "^~") e->op = "~^";
      e->args.push_back(std::move(lhs));
      e->args.push_back(binary(p + 1));  // +1: left associative
      lhs = std::move(e);
    }
  }

  ExprP unary() {
    static const std::set<std::string> un = {"~", "!", "&", "|", "^", "~&", "~|", "~^", "-", "+"};
    if (cur().kind == Tok::Sym && un.count(cur().text)) {
      auto e = node(Expr::Unary);
      e->op = t_[i_++].text;
      e->args.push_back(unary());
      if (e->op == "+") return std::move(e->args[0]);
      return e;
    }
    return primary();
  }

  ExprP primary() {
    if (accept("(")) {
      ExprP e = expr();
      expect(")");
      return e;
    }
    if (cur().kind == Tok::Num) {
      auto e = node(Expr::Number);
      e->bits = t_[i_++].bits;
      return e;
    }
    if (accept("{")) {
      ExprP first = expr();
      if (accept("{")) {  // replication {n{...}}
        auto r = node(Expr::Repl);
        auto inner = node(Expr::Concat);
        do inner->args.push_back(expr());
        while (accept(","));
        expect("}");
        expect("}");
        r->args.push_back(std::move(first));
        r->args.push_back(std::move(inner));
        return r;
      }
      auto c = node(Expr::Concat);
      c->args.push_back(std::move(first));
      while (accept(",")) c->args.push_back(expr());
      expect("}");
      return c;
    }
    if (cur().kind == Tok::Id) return ident_ref();
    fail("unexpected '" + cur().text + "' in expression");
  }

  std::vector<Tok> t_;
  std::string file_;
  size_t i_ = 0;
};

}  // namespace

std::vector<Module> parse_text(const std::string& text, const std::string& filename) {
  Lexer lx(text, filename);
  Parser p(lx.run(), filename);
  return p.run();
}

std::vector<Module> parse_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_text(ss.str(), path);
}

}  // namespace lf

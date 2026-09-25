#pragma once

#include <memory>
#include <string>
#include <vector>

namespace lf {

struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
  enum Kind { Ident, Number, Index, Slice, Unary, Binary, Ternary, Concat, Repl };
  Kind kind;
  int line = 0;
  std::string name;        // Ident/Index/Slice
  std::string op;          // Unary/Binary
  std::vector<bool> bits;  // Number, LSB first
  // Index: [idx]  Slice: [msb, lsb]  Unary: [a]  Binary: [a, b]
  // Ternary: [cond, then, else]  Concat: parts, MSB first  Repl: [count, concat]
  std::vector<ExprP> args;
};

ExprP clone(const Expr& e);

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Stmt {
  enum Kind { Block, Assign, If } kind;
  int line = 0;
  ExprP lhs, rhs, cond;
  bool nonblocking = false;
  std::vector<StmtP> body;    // Block: statements, If: then-branch (0 or 1 stmt)
  std::vector<StmtP> orelse;  // If: else-branch (0 or 1 stmt)
};

struct Decl {
  enum Dir { In, Out, Internal } dir;
  std::string name;
  ExprP msb, lsb;  // null for scalars
  int line = 0;
};

struct Param {
  std::string name;
  ExprP value;
  int line = 0;
};

struct ContAssign {
  ExprP lhs, rhs;
  int line = 0;
};

struct Always {
  bool ff = false;
  std::string clock;
  StmtP body;
  int line = 0;
};

struct Module {
  std::string name;
  std::string file;
  int line = 0;
  std::vector<std::string> port_order;
  std::vector<Param> params;
  std::vector<Decl> decls;
  std::vector<ContAssign> assigns;
  std::vector<Always> blocks;
};

std::vector<Module> parse_text(const std::string& text, const std::string& filename);
std::vector<Module> parse_file(const std::string& path);

}  // namespace lf

// Unit tests: elaboration semantics vs plain C++ arithmetic, one test per pass,
// front-end errors, and the thread pool.

#include <atomic>

#include "check.h"
#include "flow.h"
#include "passes.h"
#include "pool.h"

using namespace lf;

// ---- semantics: the elaborated graph must compute what Verilog says -------

TEST(numbers) {
  Graph g = build(R"(
    module m(output logic [7:0] a, b, c, d, output logic [11:0] e);
      assign a = 8'hA5;
      assign b = 8'b1010_0101;
      assign c = 'o245;
      assign d = 165;
      assign e = 12'd4095;
    endmodule)");
  auto o = eval(g, {});
  CHECK_EQ(o["a"], 0xA5u);
  CHECK_EQ(o["b"], 0xA5u);
  CHECK_EQ(o["c"], 0xA5u);
  CHECK_EQ(o["d"], 0xA5u);
  CHECK_EQ(o["e"], 4095u);
}

TEST(adder_matches_integer_add) {
  Graph g = build(R"(
    module add(input logic [3:0] a, b, input logic cin, output logic [3:0] s, output logic co);
      assign {co, s} = a + b + cin;
    endmodule)");
  for (uint64_t a = 0; a < 16; a++)
    for (uint64_t b = 0; b < 16; b++)
      for (uint64_t c = 0; c < 2; c++) {
        auto o = eval(g, {{"a", a}, {"b", b}, {"cin", c}});
        CHECK_EQ(o["s"], (a + b + c) & 15);
        CHECK_EQ(o["co"], (a + b + c) >> 4);
      }
}

TEST(sub_mul_neg) {
  Graph g = build(R"(
    module m(input logic [3:0] a, b, output logic [3:0] d, n, output logic [7:0] p);
      assign d = a - b;
      assign n = -a;
      assign p = a * b;
    endmodule)");
  for (uint64_t a = 0; a < 16; a++)
    for (uint64_t b = 0; b < 16; b++) {
      auto o = eval(g, {{"a", a}, {"b", b}});
      CHECK_EQ(o["d"], (a - b) & 15);
      CHECK_EQ(o["n"], (0 - a) & 15);
      CHECK_EQ(o["p"], a * b);
    }
}

TEST(comparisons) {
  Graph g = build(R"(
    module m(input logic [3:0] a, b, output logic lt, le, gt, ge, eq, ne);
      assign lt = a < b;  assign le = a <= b;  assign gt = a > b;
      assign ge = a >= b; assign eq = a == b;  assign ne = a != b;
    endmodule)");
  for (uint64_t a = 0; a < 16; a++)
    for (uint64_t b = 0; b < 16; b++) {
      auto o = eval(g, {{"a", a}, {"b", b}});
      CHECK_EQ(o["lt"], uint64_t(a < b));
      CHECK_EQ(o["le"], uint64_t(a <= b));
      CHECK_EQ(o["gt"], uint64_t(a > b));
      CHECK_EQ(o["ge"], uint64_t(a >= b));
      CHECK_EQ(o["eq"], uint64_t(a == b));
      CHECK_EQ(o["ne"], uint64_t(a != b));
    }
}

TEST(reductions_shifts_concat) {
  Graph g = build(R"(
    module m(input logic [5:0] x, output logic r_and, r_or, r_xor, r_xnor, lnot,
             output logic [5:0] shl, shr, output logic [11:0] rep);
      assign r_and = &x;  assign r_or = |x;  assign r_xor = ^x;  assign r_xnor = ~^x;
      assign lnot = !x;
      assign shl = x << 2;
      assign shr = x >> 1;
      assign rep = {2{x[5:3], x[2:0]}};
    endmodule)");
  for (uint64_t x = 0; x < 64; x++) {
    auto o = eval(g, {{"x", x}});
    CHECK_EQ(o["r_and"], uint64_t(x == 63));
    CHECK_EQ(o["r_or"], uint64_t(x != 0));
    CHECK_EQ(o["r_xor"], uint64_t(__builtin_popcountll(x) & 1));
    CHECK_EQ(o["r_xnor"], uint64_t(!(__builtin_popcountll(x) & 1)));
    CHECK_EQ(o["lnot"], uint64_t(x == 0));
    CHECK_EQ(o["shl"], (x << 2) & 63);
    CHECK_EQ(o["shr"], x >> 1);
    CHECK_EQ(o["rep"], (x << 6) | x);
  }
}

TEST(always_comb_case_and_if) {
  Graph g = build(R"(
    module m(input logic [1:0] op, input logic [3:0] a, b, input logic inv, output logic [3:0] y);
      always_comb begin
        case (op)
          2'd0: y = a & b;
          2'd1: y = a | b;
          2'd2, 2'd3: y = a ^ b;
          default: y = 4'd0;  // unreachable, but without it every case counts as a latch
        endcase
        if (inv) y = ~y;
      end
    endmodule)");
  for (uint64_t op = 0; op < 4; op++)
    for (uint64_t a = 0; a < 16; a++)
      for (uint64_t b = 0; b < 16; b++)
        for (uint64_t inv = 0; inv < 2; inv++) {
          uint64_t y = op == 0 ? (a & b) : op == 1 ? (a | b) : (a ^ b);
          if (inv) y = ~y & 15;
          CHECK_EQ(eval(g, {{"op", op}, {"a", a}, {"b", b}, {"inv", inv}})["y"], y);
        }
}

TEST(always_ff_next_state) {
  Graph g = build(R"(
    module m(input logic clk, rst, en, output logic [2:0] q);
      always_ff @(posedge clk)
        if (rst) q <= 3'd0;
        else if (en) q <= q + 3'd1;
    endmodule)");
  CHECK_EQ(g.flops.size(), size_t(3));
  for (uint64_t q = 0; q < 8; q++)
    for (uint64_t rst = 0; rst < 2; rst++)
      for (uint64_t en = 0; en < 2; en++) {
        auto o = eval(g, {{"rst", rst}, {"en", en}}, {{"q", q}});
        uint64_t want = rst ? 0 : en ? (q + 1) & 7 : q;
        CHECK_EQ(o["q.d"], want);
        CHECK_EQ(o["q"], q);  // the port shows the current value
      }
}

TEST(parameters) {
  Graph g = build(R"(
    module m #(parameter W = 4, parameter K = 3) (input logic [W-1:0] a, output logic [W-1:0] y);
      localparam OFF = K * 2;
      assign y = a + OFF;
    endmodule)");
  CHECK_EQ(g.inputs.size(), size_t(4));
  for (uint64_t a = 0; a < 16; a++) CHECK_EQ(eval(g, {{"a", a}})["y"], (a + 6) & 15);
}

// ---- passes ---------------------------------------------------------------

static size_t gates(const Graph& g) { return measure(g).gates; }

TEST(constprop_folds_constants) {
  Graph g = build(R"(
    module m(input logic a, b, output logic y);
      assign y = (a & 1'b0) | (b & 1'b1);
    endmodule)");
  const_prop(g);
  CHECK_EQ(gates(g), size_t(0));
  CHECK_EQ(g.outputs[0].node, g.inputs[1].node);  // y is just b
}

TEST(simplify_identities) {
  Graph g = build(R"(
    module m(input logic a, b, s, output logic y0, y1, y2, y3, y4);
      assign y0 = a & ~a;
      assign y1 = ~~a;
      assign y2 = a | (a & b);
      assign y3 = s ? b : b;
      assign y4 = a ^ a;
    endmodule)");
  simplify(g);
  dce(g);
  CHECK_EQ(g.outputs[0].node, kFalse);
  CHECK_EQ(g.outputs[1].node, g.inputs[0].node);
  CHECK_EQ(g.outputs[2].node, g.inputs[0].node);
  CHECK_EQ(g.outputs[3].node, g.inputs[1].node);
  CHECK_EQ(g.outputs[4].node, kFalse);
  CHECK_EQ(gates(g), size_t(0));
}

TEST(strash_merges_duplicates) {
  Graph g = build(R"(
    module m(input logic a, b, output logic y, z);
      assign y = a & b;
      assign z = b & a;
    endmodule)");
  CHECK_EQ(gates(g), size_t(2));
  strash(g);
  CHECK_EQ(gates(g), size_t(1));
  CHECK_EQ(g.outputs[0].node, g.outputs[1].node);
}

TEST(dce_removes_unused_logic_and_flops) {
  Graph g = build(R"(
    module m(input logic clk, input logic [3:0] a, b, output logic y);
      logic [3:0] junk;
      logic [3:0] r;
      assign junk = a + b;
      always_ff @(posedge clk) r <= a;
      assign y = a[0];
    endmodule)");
  CHECK(gates(g) > 0);
  CHECK_EQ(g.flops.size(), size_t(4));
  dce(g);
  CHECK_EQ(gates(g), size_t(0));
  CHECK_EQ(g.flops.size(), size_t(0));
  CHECK_EQ(g.inputs.size(), size_t(9));  // ports are kept even if unused
}

TEST(balance_cuts_depth) {
  Graph g = build(R"(
    module m(input logic [7:0] x, output logic y);
      assign y = &x;
    endmodule)");
  CHECK_EQ(measure(g).depth, 7);
  balance(g);
  CHECK_EQ(measure(g).depth, 3);
  CHECK_EQ(gates(g), size_t(7));
}

TEST(sweep_finds_functional_duplicates) {
  Graph g = build(R"(
    module m(input logic a, b, c, output logic y, z);
      assign y = (a & b) | (a & c);
      assign z = a & (b | c);
    endmodule)");
  strash(g);
  CHECK(g.outputs[0].node != g.outputs[1].node);  // structurally different
  sweep(g, 10);
  dce(g);
  CHECK_EQ(g.outputs[0].node, g.outputs[1].node);
  CHECK_EQ(gates(g), size_t(3));  // keeps the first version it saw, not the cheapest
}

TEST(sweep_merges_into_inputs) {
  Graph g = build(R"(
    module m(input logic [2:0] bin, output logic [2:0] back);
      logic [2:0] gray;
      assign gray = bin ^ (bin >> 1);
      assign back[2] = gray[2];
      assign back[1] = back[2] ^ gray[1];
      assign back[0] = back[1] ^ gray[0];
    endmodule)");
  sweep(g, 10);
  dce(g);
  CHECK_EQ(gates(g), size_t(0));  // back is bin
  for (int i = 0; i < 3; i++) CHECK_EQ(g.outputs[i].node, g.inputs[i].node);
}

TEST(sweep_respects_support_limit) {
  Graph g = build(R"(
    module m(input logic a, b, c, output logic y, z);
      assign y = (a & b) | (a & c);
      assign z = a & (b | c);
    endmodule)");
  sweep(g, 2);  // needs 3 inputs to prove, so no merge
  dce(g);
  CHECK(g.outputs[0].node != g.outputs[1].node);
}

TEST(regions_are_deterministic) {
  Graph g = build(R"(
    module m(input logic [7:0] a, b, output logic [7:0] s, x, output logic p);
      assign s = a + b;
      assign x = a ^ b;
      assign p = ^a;
    endmodule)");
  FlowOptions o1, o4;
  o1.threads = 1;
  o4.threads = 4;
  FlowStats st;
  Graph r1 = optimize_design(g, o1, &st);
  Graph r4 = optimize_design(g, o4);
  CHECK(st.regions >= 3);
  CHECK(dump(r1) == dump(r4));
  CHECK(check_equivalence(g, r1).equal);
}

// ---- errors ---------------------------------------------------------------

TEST(front_end_errors) {
  CHECK(throws_with("module m(input logic a, b, output logic y);\n assign y = a;\n assign y = b;\nendmodule",
                    "more than one driver"));
  CHECK(throws_with("module m(input logic a, output logic y);\nendmodule", "never driven"));
  CHECK(throws_with("module m(input logic a, b, output logic y);\n logic x;\n assign x = y & a;\n assign y = x | b;\n"
                    "endmodule",
                    "loop"));
  CHECK(throws_with("module m(input logic a, s, output logic y);\n always_comb if (s) y = a;\nendmodule", "latch"));
  // no coverage analysis: a case without default is a latch even when it happens to be full
  CHECK(throws_with("module m(input logic s, output logic y);\n always_comb case (s) 1'b0: y = 1'b1; 1'b1: y = 1'b0; "
                    "endcase\nendmodule",
                    "latch"));
  CHECK(throws_with("module m(input logic a, output logic y);\n assign y = q;\nendmodule", "test.sv:2: undeclared"));
  CHECK(throws_with("module m(output logic [3:0] y);\n assign y = 4'bx01z;\nendmodule", "x/z"));
  CHECK(throws_with("module m(input logic a, output logic y);\n sub u(a, y);\nendmodule", "module instances"));
  CHECK(throws_with("module m(input logic a, output logic y);\n assign a = 1'b0;\n assign y = a;\nendmodule",
                    "cannot assign to input"));
}

// ---- pool -----------------------------------------------------------------

TEST(pool_runs_every_task_once) {
  StealPool pool(8);
  std::vector<std::atomic<int>> hits(5000);
  std::vector<size_t> order(hits.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  pool.run(order, [&](size_t t, int) { hits[t]++; });
  size_t ran = 0;
  for (const auto& s : pool.stats()) ran += s.ran;
  CHECK_EQ(ran, hits.size());
  for (auto& h : hits) CHECK_EQ(h.load(), 1);
}

TEST(pool_rethrows) {
  StealPool pool(4);
  std::vector<size_t> order = {0, 1, 2, 3, 4, 5, 6, 7};
  bool caught = false;
  try {
    pool.run(order, [](size_t t, int) {
      if (t == 5) throw std::runtime_error("boom");
    });
  } catch (const std::runtime_error&) {
    caught = true;
  }
  CHECK(caught);
}

int main() { return run_all(); }

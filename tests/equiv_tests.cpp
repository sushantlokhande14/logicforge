// End-to-end checks on every design in tests/designs plus a few hundred random ones:
//   1. optimized netlist is equivalent to the elaborated one
//   2. -j1 and -j4 produce byte-identical netlists
//   3. region flow and whole-graph flow agree functionally
//   4. the written Verilog parses back into an equivalent design

#include <algorithm>
#include <dirent.h>
#include <random>
#include <sstream>

#include "check.h"
#include "flow.h"
#include "writer.h"

using namespace lf;

static bool check_design(const std::string& label, const Graph& elab) {
  bool ok = true;
  auto fail = [&](const std::string& what) {
    std::fprintf(stderr, "%s: %s\n", label.c_str(), what.c_str());
    g_failures++;
    ok = false;
  };

  FlowOptions serial, par, whole;
  serial.threads = 1;
  par.threads = 4;
  whole.regions = false;
  Graph a = optimize_design(elab, serial);
  Graph b = optimize_design(elab, par);
  Graph c = optimize_design(elab, whole);
  verify(a);

  auto eq = check_equivalence(elab, a);
  if (!eq.equal) fail("optimized != elaborated: " + eq.detail);
  std::string va = write_verilog(a);
  if (va != write_verilog(b)) fail("-j1 and -j4 netlists differ");
  eq = check_equivalence(elab, c);
  if (!eq.equal) fail("whole-graph flow != elaborated: " + eq.detail);

  // round trip through our own writer and parser
  Graph back = build(va);
  for (auto& f : back.flops)
    if (f.name.size() > 3 && f.name.compare(f.name.size() - 3, 3, "__q") == 0) f.name.resize(f.name.size() - 3);
  eq = check_equivalence(elab, back);
  if (!eq.equal) fail("written netlist != elaborated: " + eq.detail);
  return ok;
}

// ---- random designs ---------------------------------------------------------

struct Fuzz {
  std::mt19937 rng;
  explicit Fuzz(unsigned seed) : rng(seed) {}
  int pick(int n) { return int(rng() % unsigned(n)); }

  std::string leaf() {
    switch (pick(6)) {
      case 0: return "a";
      case 1: return "b";
      case 2: return "a[" + std::to_string(pick(4)) + "]";
      case 3: return "b[" + std::to_string(pick(2) + 2) + ":" + std::to_string(pick(2)) + "]";
      case 4: return "c";
      default: return std::to_string(pick(4) + 1) + "'d" + std::to_string(pick(8));
    }
  }

  std::string expr(int depth) {
    if (depth == 0 || pick(4) == 0) return pick(5) == 0 ? "s" : leaf();
    static const char* bin[] = {"&", "|", "^", "~^", "+", "-", "*", "==", "!=", "<", ">", "<=", ">=", "&&", "||"};
    static const char* un[] = {"~", "-", "&", "|", "^", "!"};
    switch (pick(6)) {
      case 0: return std::string(un[pick(6)]) + "(" + expr(depth - 1) + ")";
      case 1: return "(" + expr(depth - 1) + " ? " + expr(depth - 1) + " : " + expr(depth - 1) + ")";
      case 2: return "{" + expr(depth - 1) + ", " + leaf() + "}";
      case 3: return "(" + expr(depth - 1) + (pick(2) ? " << " : " >> ") + std::to_string(pick(3)) + ")";
      default: return "(" + expr(depth - 1) + " " + bin[pick(15)] + " " + expr(depth - 1) + ")";
    }
  }

  std::string module() {
    std::ostringstream os;
    os << "module fuzz(input logic clk, input logic [3:0] a, input logic [3:0] b, input logic [1:0] c,\n"
          "            input logic s, output logic [3:0] y0, output logic [3:0] y1, output logic y2);\n"
          "  logic [2:0] r;\n  logic [3:0] dead;\n";
    os << "  assign y0 = " << expr(4) << ";\n";
    os << "  assign dead = " << expr(3) << ";\n";
    os << "  always_comb begin\n    y1 = " << expr(3) << ";\n";
    os << "    if (" << expr(2) << ") y1 = " << expr(3) << ";\n  end\n";
    os << "  always_ff @(posedge clk)\n    if (" << expr(2) << ") r <= " << expr(3) << ";\n    else r <= r + 3'd1;\n";
    os << "  assign y2 = ^r ^ " << (pick(2) ? "(" + expr(2) + " != 4'd0)" : "s") << ";\n";
    os << "endmodule\n";
    return os.str();
  }
};

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "tests/designs";
  std::vector<std::string> files;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 3 && n.substr(n.size() - 3) == ".sv") files.push_back(dir + "/" + n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::fprintf(stderr, "no designs found in %s\n", dir.c_str());
    return 1;
  }
  for (const auto& f : files) {
    try {
      auto mods = parse_file(f);
      Graph elab = elaborate(mods.back());
      bool ok = check_design(f, elab);
      std::printf("%s %s\n", ok ? "ok  " : "FAIL", f.c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", f.c_str(), e.what());
      g_failures++;
    }
  }

  int fuzz_ok = 0;
  const int kSeeds = 300;
  for (int seed = 1; seed <= kSeeds; seed++) {
    Fuzz fz(seed);
    std::string src = fz.module();
    try {
      Graph elab = build(src);
      if (check_design("fuzz seed " + std::to_string(seed), elab)) fuzz_ok++;
      else std::fprintf(stderr, "---- source ----\n%s\n", src.c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fuzz seed %d: %s\n%s\n", seed, e.what(), src.c_str());
      g_failures++;
    }
  }
  std::printf("fuzz: %d/%d random designs passed\n", fuzz_ok, kSeeds);
  std::printf("%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}

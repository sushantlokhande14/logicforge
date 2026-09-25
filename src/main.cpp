// lfc: the LogicForge command line driver.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "elaborate.h"
#include "flow.h"
#include "sim.h"
#include "writer.h"

using namespace lf;

namespace {

const char* kUsage = R"(usage: lfc [options] <file.sv>...

  --top NAME          module to compile (default: the last one in the input)
  -j N                worker threads (default: all hardware threads)
  -O0                 elaborate only, skip optimization
  --no-regions        optimize the whole graph as one unit
  --no-balance        skip depth balancing
  --no-sweep          skip functional sweeping
  --sweep-support K   max inputs for an exact sweep check (default 10)
  --skip P1,P2        leave passes out (constprop, simplify, strash, dce,
                      balance, sweep), handy for finding which one broke something
  -o FILE             write the optimized netlist as Verilog
  --dot FILE          write the optimized graph as graphviz
  --dump-ir           print the optimized IR
  --verify            check optimized == elaborated
  --verify-each       run the IR checker after every pass
  --stats             print QoR before/after
  --profile           print time spent in each stage
  --json FILE         write stats + profile as JSON
)";

struct Args {
  std::vector<std::string> files;
  std::string top, out, dot, json;
  int threads = 0;
  bool opt = true, dump = false, verify = false, stats = false, profile = false;
  FlowOptions flow;
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    std::string s = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "lfc: " << s << " needs a value\n";
        std::exit(1);
      }
      return argv[++i];
    };
    if (s == "--top") a.top = next();
    else if (s == "-j") a.threads = std::atoi(next().c_str());
    else if (s.rfind("-j", 0) == 0 && s.size() > 2) a.threads = std::atoi(s.c_str() + 2);
    else if (s == "-O0") a.opt = false;
    else if (s == "--no-regions") a.flow.regions = false;
    else if (s == "--no-balance") a.flow.passes.balance = false;
    else if (s == "--no-sweep") a.flow.passes.sweep = false;
    else if (s == "--sweep-support") a.flow.passes.sweep_support = std::atoi(next().c_str());
    else if (s == "--skip") {
      std::stringstream ss(next());
      for (std::string p; std::getline(ss, p, ',');) a.flow.passes.skip.push_back(p);
    }
    else if (s == "-o") a.out = next();
    else if (s == "--dot") a.dot = next();
    else if (s == "--dump-ir") a.dump = true;
    else if (s == "--verify") a.verify = true;
    else if (s == "--verify-each") a.flow.passes.verify_each = true;
    else if (s == "--stats") a.stats = true;
    else if (s == "--profile") a.profile = true;
    else if (s == "--json") a.json = next();
    else if (s == "-h" || s == "--help") {
      std::cout << kUsage;
      std::exit(0);
    } else if (!s.empty() && s[0] == '-') {
      std::cerr << "lfc: unknown option " << s << "\n" << kUsage;
      std::exit(1);
    } else a.files.push_back(s);
  }
  if (a.files.empty()) {
    std::cerr << kUsage;
    std::exit(1);
  }
  if (a.threads <= 0) a.threads = (int)std::max(1u, std::thread::hardware_concurrency());
  a.flow.threads = a.threads;
  if (a.flow.passes.sweep_support > 16) a.flow.passes.sweep_support = 16;
  return a;
}

struct Stage {
  std::string name;
  double ms;
};

void write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f << text;
}

std::string qor_json(const Qor& q) {
  std::ostringstream os;
  os << "{\"gates\": " << q.gates << ", \"area\": " << q.area << ", \"and\": " << q.ands << ", \"or\": " << q.ors << ", \"xor\": " << q.xors
     << ", \"not\": " << q.nots << ", \"mux\": " << q.muxes << ", \"depth\": " << q.depth << ", \"flops\": " << q.flops
     << ", \"inputs\": " << q.inputs << ", \"outputs\": " << q.outputs << "}";
  return os.str();
}

}  // namespace

int main(int argc, char** argv) {
#ifdef __GLIBC__
  // Every pass allocates a fresh graph and frees the old one, so each thread's
  // malloc arena keeps growing and trimming. Growing/trimming takes the
  // process-wide mmap lock, and at -j16 threads spent their time waiting on it
  // (16k page faults, 1.5k context switches per run). Never give memory back
  // mid-run instead: faults drop ~20x and the region phase scales again.
  mallopt(M_TRIM_THRESHOLD, 1 << 30);
  mallopt(M_TOP_PAD, 64 << 20);
  mallopt(M_MMAP_THRESHOLD, 1 << 30);
#endif
  Args args = parse_args(argc, argv);
  std::vector<Stage> stages;
  auto t_all = std::chrono::steady_clock::now();
  auto timed = [&](const char* name, auto&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    stages.push_back({name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()});
  };

  try {
    std::vector<Module> mods;
    timed("parse", [&] {
      for (const auto& f : args.files) {
        auto m = parse_file(f);
        for (auto& x : m) mods.push_back(std::move(x));
      }
    });
    if (mods.empty()) throw std::runtime_error("no modules found");
    const Module* top = &mods.back();
    if (!args.top.empty()) {
      top = nullptr;
      for (const auto& m : mods)
        if (m.name == args.top) top = &m;
      if (!top) throw std::runtime_error("no module named " + args.top);
    }

    Graph elab;
    timed("elaborate", [&] { elab = elaborate(*top); });
    verify(elab);
    Qor before = measure(elab);

    Graph opt;
    FlowStats fs;
    if (args.opt) timed("optimize", [&] { opt = optimize_design(elab, args.flow, &fs); });
    else opt = elab;
    verify(opt);
    Qor after = measure(opt);

    EquivResult eq;
    if (args.verify) timed("verify", [&] { eq = check_equivalence(elab, opt); });
    if (!args.out.empty()) timed("write", [&] { write_file(args.out, write_verilog(opt)); });
    if (!args.dot.empty()) write_file(args.dot, write_dot(opt));
    if (args.dump) std::cout << dump(opt);
    double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_all).count();

    if (args.stats) {
      auto row = [](const char* what, const Qor& q) {
        std::printf("  %-10s gates %8zu  area %8zu  depth %4d  flops %zu  (and %zu, or %zu, xor %zu, not %zu, mux %zu)\n",
                    what, q.gates, q.area, q.depth, q.flops, q.ands, q.ors, q.xors, q.nots, q.muxes);
      };
      auto pct = [](size_t a, size_t b) { return a ? 100.0 * (double(b) - double(a)) / double(a) : 0.0; };
      std::printf("%s: %zu inputs, %zu outputs\n", top->name.c_str(), before.inputs, before.outputs);
      row("elaborated", before);
      row("optimized", after);
      std::printf("  gates %+.1f%%, area %+.1f%%, depth %d -> %d\n", pct(before.gates, after.gates),
                  pct(before.area, after.area), before.depth, after.depth);
      if (fs.dead_regions)
        std::printf("    %-10s dropped %8zu gates in %zu regions nothing reads\n", "partition", fs.dead_gates,
                    fs.dead_regions);
      for (const auto& p : fs.passes)
        std::printf("    %-10s removed %8ld gates (%d runs)\n", p.name.c_str(), p.gates_removed, p.runs);
    }
    if (args.profile) {
      std::printf("profile (%d threads):\n", args.threads);
      for (const auto& s : stages) std::printf("  %-10s %9.2f ms\n", s.name.c_str(), s.ms);
      if (args.opt) {
        std::printf("    partition %9.2f ms  (%zu regions, %zu dead, largest %zu gates)\n", fs.ms_partition, fs.regions,
                    fs.dead_regions, fs.largest_region);
        std::printf("    regions   %9.2f ms\n    merge     %9.2f ms\n    cleanup   %9.2f ms\n", fs.ms_optimize,
                    fs.ms_merge, fs.ms_cleanup);
        std::printf("  pass cpu time (summed over threads):\n");
        for (const auto& p : fs.passes) std::printf("    %-10s %9.2f ms\n", p.name.c_str(), p.ms);
        for (size_t w = 0; w < fs.workers.size(); w++)
          std::printf("    worker %-2zu ran %6zu stole %6zu\n", w, fs.workers[w].ran, fs.workers[w].stolen);
      }
      std::printf("  %-10s %9.2f ms  (%.0f elaborated gates/s)\n", "total", total,
                  total > 0 ? before.gates / (total / 1000.0) : 0.0);
    }
    if (!args.json.empty()) {
      std::ostringstream js;
      js << "{\n  \"module\": \"" << top->name << "\",\n  \"threads\": " << args.threads << ",\n  \"regions\": " << fs.regions
         << ",\n  \"before\": " << qor_json(before) << ",\n  \"after\": " << qor_json(after) << ",\n  \"total_ms\": " << total
         << ",\n  \"stages\": {";
      for (size_t i = 0; i < stages.size(); i++)
        js << (i ? ", " : "") << "\"" << stages[i].name << "\": " << stages[i].ms;
      js << "},\n  \"passes\": {";
      for (size_t i = 0; i < fs.passes.size(); i++)
        js << (i ? ", " : "") << "\"" << fs.passes[i].name << "\": {\"removed\": " << fs.passes[i].gates_removed
           << ", \"ms\": " << fs.passes[i].ms << "}";
      js << "}";
      if (args.verify) js << ",\n  \"equivalent\": " << (eq.equal ? "true" : "false");
      js << "\n}\n";
      write_file(args.json, js.str());
    }
    if (args.verify) {
      std::printf("verify: %s (%s, %llu patterns)\n", eq.equal ? "equivalent" : "MISMATCH",
                  eq.exhaustive ? "exhaustive" : "random", (unsigned long long)eq.patterns);
      if (!eq.equal) {
        std::printf("  %s\n", eq.detail.c_str());
        return 2;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "lfc: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

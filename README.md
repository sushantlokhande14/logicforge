# LogicForge

A small parallel RTL compiler in C++17. `lfc` reads a synthesizable subset of
Verilog/SystemVerilog, bit-blasts it into a gate graph, optimizes that graph
on all cores, and writes out a gate-level Verilog netlist. It checks the result
against the original with bit-parallel simulation.

```
 .sv ──> parse ──> elaborate ──> partition ──> optimize regions ──> merge ──> netlist.v
         (AST)     (gate graph)  (union-find)  (work-stealing pool) (strash)
                                                 constprop
                                                 simplify
                                                 strash
                                                 dce
                                                 balance
                                                 sweep
```

It's a weekend-sized project, not a replacement for Yosys or ABC. The point is
a pipeline small enough to read in one sitting, where every step can be
measured and verified.

## Build

Needs a C++17 compiler and CMake 3.16+. Linux or WSL; nothing else to install.

```bash
cmake --preset release
cmake --build build/release -j
ctest --test-dir build/release
```

## Try it

```bash
./build/release/lfc examples/tiny.sv --stats --verify -o tiny_opt.v
```

```
tiny: 5 inputs, 3 outputs
  elaborated gates       45  area       51  depth   34  flops 0
  optimized  gates        3  area        3  depth    2  flops 0
  gates -93.3%, area -94.1%, depth 34 -> 2
verify: equivalent (exhaustive, 32 patterns)
```

Useful flags: `--profile` (time per stage and per pass), `--dump-ir`, `--skip PASS`,
`--dot g.dot` (Graphviz), `-O0` (elaborate only), `-j N`, `--json stats.json`.
Run `lfc --help` for the full list.

## What it does

| Stage | What happens |
|---|---|
| parse | hand-written lexer + recursive-descent parser, precedence climbing for expressions |
| elaborate | params, widths, `assign`, `always_comb`, `always_ff`, `if`/`case` become 1-bit gates. `+ - * < ==` are bit-blasted into ripple-carry and array circuits |
| constprop | `x & 0 = 0`, `mux(1, a, b) = a`, `x ^ 1 = ~x` ... |
| simplify | Boolean identities: `x & ~x = 0`, `~~x = x`, `a \| (a & b) = a`, mux rules |
| strash | structural hashing: two gates with the same op and inputs become one |
| dce | removes logic no output depends on, including flops nobody reads |
| balance | rebuilds AND/OR/XOR chains as minimum-depth trees |
| sweep | finds gates that compute the *same function* with different structure. Candidates come from random simulation and are proven by exhaustive simulation over their support (up to 10 inputs by default) |

Flops are handled the standard way for combinational optimization: a flop's
output is a free input and its D pin is an extra output.

## Parallelism

After elaboration the graph is split with union-find into **independent logic
regions**: groups of gates that don't share any gate with another group. They
can share primary inputs, just not logic. Each region is copied into its own
small graph and optimized on a work-stealing thread pool. A worker takes from
the front of its own deque and steals from the back of others'. The results are
stitched back in region order, with hash-consing to merge identical logic that
ended up in two regions. So the output is byte-identical for any `-j`, and the
tests and the benchmark script both check that.

Measured on an Intel Core Ultra 9 185H (16 cores: 6P + 8E + 2LP-E, 22
threads), Ubuntu 24.04 in Docker on WSL2, GCC 13, Release build. Medians of
7 runs, design from `tools/gen_bench.py --blocks 2000` (836k gates elaborated):

| threads | total ms | speedup | optimize stage ms | speedup |
|---:|---:|---:|---:|---:|
| 1 | 328.8 | 1.00x | 214.9 | 1.00x |
| 4 | 185.3 | 1.77x | 88.0 | 2.44x |
| 8 | 175.6 | 1.87x | 70.0 | 3.07x |
| 16 | 159.1 | 2.07x | 62.0 | 3.47x |

The optimize stage scales 3.0-3.5x at 16 threads on all three benchmark designs
(the region phase inside it, about 7x). End to end it's about 2x, because parse,
elaborate, and writing the netlist are still serial: Amdahl's law in action.
Raw numbers are in [docs/bench](docs/bench), and [docs/performance.md](docs/performance.md)
covers method and profiling notes (including a malloc problem that made `-j16`
slower than `-j8`).

## Testing

`ctest` runs three suites:

- **unit**: elaboration semantics checked against plain C++ arithmetic
  (exhaustive over small widths), one test per pass, front-end errors, the pool.
- **equiv**: every design in `tests/designs` plus 300 random fuzz designs:
  optimized is equivalent to elaborated (exhaustive, since they have 16 or
  fewer free inputs), `-j1` and `-j4` netlists are byte-identical, the
  whole-graph flow agrees, and the written netlist parses back to an equivalent
  design.
- **qor**: gates/area/depth/flops for each design must match
  `tests/golden/qor.txt` exactly. Any change, better or worse, fails until the
  golden file is refreshed on purpose.

Sanitizer builds: `cmake --preset asan` (address + undefined) and
`cmake --preset tsan`. Debugging notes are in [docs/debugging.md](docs/debugging.md).

## Docs

- [walkthrough.md](docs/walkthrough.md): one small design through every stage, with the real IR after each pass
- [design.md](docs/design.md): IR invariants, how each pass works, regions, work stealing, determinism
- [language.md](docs/language.md): the accepted Verilog/SystemVerilog subset
- [performance.md](docs/performance.md): benchmark method, results, and what profiling found
- [debugging.md](docs/debugging.md): sanitizers, gdb, and bisecting correctness/QoR/perf regressions

## Layout

```
src/
  parser.cpp     lexer + parser -> AST (ast.h)
  elaborate.cpp  AST -> gate graph (ir.h)
  builder.h      node factory: folding, rewrites, strash table
  passes.cpp     the optimization passes and the fixpoint driver
  flow.cpp       partitioning into regions, parallel optimize, merge
  pool.h         work-stealing thread pool
  sim.cpp        bit-parallel simulation and equivalence checking
  writer.cpp     Verilog and Graphviz output
  main.cpp       the lfc driver
tests/           unit, equivalence/fuzz and QoR regression suites
tools/           benchmark generator, thread-scaling script, QoR diff
docs/            design notes, walkthrough, language subset, performance, debugging
examples/        tiny.sv (used in the walkthrough) and a language tour
```

## Limits

- One module at a time. No instances or hierarchy, so flatten first.
- No `generate`, functions, `casez`, x/z values, or async resets.
  Variable bit-selects (`a[i]`) aren't supported, only constant ones.
- Everything is unsigned.
- `sweep` can only prove equivalences over 16 or fewer inputs. A SAT solver
  would lift that (for example, `mac.sv` has two equal outputs it can't merge).
- Equivalence checking is exhaustive up to 16 free inputs and random
  simulation above that, so for big designs it's strong evidence, not a proof.

MIT licensed.

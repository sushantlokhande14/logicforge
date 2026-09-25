# Design notes

## The IR

One node per bit, stored in a flat `std::vector<Node>`:

```cpp
enum class Op : uint8_t { Const0, Const1, Input, FlopQ, Not, And, Or, Xor, Mux };
struct Node { Op op; uint32_t in[3]; };   // Mux(s, a, b) = s ? a : b
```

Rules that every pass keeps:

- nodes 0 and 1 are the constants
- a node only reads lower-numbered nodes, so the vector is already in
  topological order
- inputs, outputs and flops are side lists pointing into the vector

`verify()` checks those rules. It runs after elaboration and after
optimization, and after every pass with `--verify-each`.

**Flops.** A flop's Q output is a source node (`FlopQ`), and its D input is
just a node id in the flop list. Seen from the gates, the design is then purely
combinational: flop outputs act like extra inputs, flop inputs like extra
outputs. That's the standard trick for combinational optimization of
sequential circuits, and it's why the passes never have to think about clocks.

Why not an AIG (only AND + complemented edges)? Keeping OR/XOR/MUX makes the
dumps and netlists readable, which matters more here than squeezing the IR.
The `area` metric still counts in AIG terms: and/or = 1, xor/mux = 3, not = 0.

## Elaboration

`elaborate.cpp` walks the AST with a symbolic value per net bit:

- **Widths.** Simplified Verilog rules: bitwise and arithmetic operators take
  the max of their operand widths and the context (e.g. the assignment's
  left-hand side). Comparisons and reductions produce 1 bit and size their
  operands on their own. That's why `{cout, sum} = a + b` keeps the carry.
- **Out-of-order assigns.** A net's value is computed on demand and memoized
  per statement. A statement that's already in progress when it's needed again
  is a combinational loop and gets reported.
- **always_comb.** The block is executed symbolically. `if/else` evaluates both
  branches from copies of the environment and merges them with muxes. If a bit
  is assigned on only one side, it would need a latch, so that's an error.
  `case` is turned into an if/else chain by the parser.
- **always_ff.** Same idea, but every register starts as its own Q value. A
  path that doesn't assign it therefore holds the old value, which is exactly
  what a flop does. Reads inside the block see the old values (nonblocking
  semantics).
- **Arithmetic.** `+`/`-` become ripple-carry adders (`a - b = a + ~b + 1`),
  `*` a shift-and-add array, `<` the inverted carry-out of `a - b`, `==` an
  AND of XNORs.

## Passes

All six are one forward sweep that builds a new graph. They share one node
factory (`Builder` in `builder.h`) with three switches:

| pass | fold | simp | hash |
|---|:-:|:-:|:-:|
| constprop | x | | |
| simplify | | x | |
| strash | | | x |
| sweep (its rebuild) | x | x | x |

- **constprop** only fires when an input is a constant. Folding can create new
  gates: `x ^ 1` becomes `~x`, `mux(s, 0, x)` becomes `~s & x`.
- **simplify** needs no constants: `x&x`, `x&~x`, `~~x`, absorption
  (`a | (a & b) = a`), and mux rules (`s ? t : t`, `~s ? a : b = s ? b : a`).
- **strash** puts commutative inputs in a canonical order (smaller id first)
  and looks every gate up in a hash table keyed by `(op, in0, in1, in2)`. A
  gate that already exists is reused. The table is open addressing and stores
  only node ids, reading keys back from the graph, so it never allocates per
  gate.
- **dce** marks everything reachable backwards from outputs. When it reaches a
  flop's Q it also marks that flop's D cone. Flops that end up unmarked are
  deleted, and so is their logic. Input ports always stay.
- **balance** finds maximal single-fanout chains of the same associative
  operator (an "AND tree") and rebuilds them Huffman-style, always combining
  the two shallowest signals. That gives minimum depth for those leaves. For
  `&x` over 8 bits, depth goes from 7 to 3 with the same 7 gates.
- **sweep** finds gates with the same function but different structure:
  1. simulate all nodes on 256 random patterns (4 x 64-bit words each);
  2. nodes whose 256-bit signatures are equal are *candidates*. Inputs are
     candidates too, which is how `(a&b)|(a&~b)` collapses onto `a`;
  3. for a candidate pair, collect the union of their supports (which sources
     they depend on). If there are at most K (default 10), simulate all 2^K
     assignments. Only if every one matches is the later node replaced by the
     earlier one.

  Random simulation is cheap but can be fooled. Exhaustive simulation is exact
  but exponential. Using the first to pick candidates and the second to prove
  them gives exact results at low cost for small supports. Real tools prove
  with a SAT solver instead, and that's the obvious next step.

`optimize()` runs the whole list until neither area nor depth improves (at
most 4 rounds).

## Parallelism

**Regions.** Union-find over gate-to-gate edges. Two gates end up in the same
region only if there is a path of gates between them. Inputs, flop outputs and
constants don't connect anything, so two cones that share only an input stay
independent. After that, every region can be optimized without looking at any
other: there's no shared gate to race on.

**Tasks.** Each live region is copied into a small standalone graph: its
sources become inputs, and the gates read from outside become outputs. Regions
nothing reads are dropped without being optimized. Tasks are sorted biggest
first and dealt round-robin to per-worker deques.

**Work stealing.** A worker pops from the front of its own deque. When that's
empty it steals from the back of another worker's deque. No task is ever added
after the start, so "I scanned every deque and they were all empty" is a safe
exit condition, with no termination protocol needed. Each deque has its own
mutex. With tasks of 100s-1000s of gates, a lock-free Chase-Lev deque wouldn't
buy anything measurable. Per-worker counters are `alignas(64)` so they don't
share a cache line.

**Determinism.** Workers finish in any order, but results are stitched back in
region-id order, and each region's work doesn't depend on the thread that ran
it. So `-j1` and `-j16` produce byte-identical netlists, and both
`equiv_tests` and `tools/bench.py` check it.

**Merge.** Copies every region back into one graph with a strash table, which
also merges identical logic that ended up in two different regions. A last
`dce` catches flops whose readers were optimized away (only visible globally).

## Equivalence checking

`sim.cpp` evaluates 64 input patterns per machine word (bit i of every word is
one pattern). Inputs are matched by port name and bit, flops by register name
and bit. Flop outputs are free variables, and flop D pins are compared like
outputs. With 16 or fewer free variables every assignment is enumerated, which
is a proof; above that it's 16k random patterns.

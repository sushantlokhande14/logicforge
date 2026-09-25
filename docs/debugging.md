# Debugging and regression hunting

## Builds

```bash
cmake --preset debug    # -O0 -g, for gdb
cmake --preset asan     # AddressSanitizer + UndefinedBehaviorSanitizer
cmake --preset tsan     # ThreadSanitizer
cmake --build build/<preset> -j && ctest --test-dir build/<preset> --output-on-failure
```

- **ASan** catches out-of-bounds and use-after-free, the likely bugs in code
  that indexes node vectors by id and reallocates them while building. Any
  report aborts the run (`-fno-sanitize-recover=all`).
- **UBSan** catches overflowing shifts and signed overflow (constant
  evaluation shifts by user-provided amounts).
- **TSan** runs the equivalence suite with `-j4`, which exercises the pool,
  the steals and the per-thread scratch. On recent kernels TSan may abort
  with "unexpected memory mapping". Run `sudo sysctl vm.mmap_rnd_bits=28`
  first, as CI does.

## gdb

```bash
gdb --args build/debug/lfc tests/designs/alu.sv -j1 --verify
(gdb) catch throw              # stop where a front-end error is raised
(gdb) break lf::sweep          # stop at a pass
(gdb) run
(gdb) call lf::debug_print(g)   # print the IR you're looking at
(gdb) p g.nodes[42]            # one node: op and fanins
(gdb) p lf::measure(g)         # gate/area/depth summary
```

- Debug with `-j1`: same output as any `-j`, and one thread to follow.
- For a hang, `thread apply all bt` shows where every worker is.
- `--dump-ir` and `--dot g.dot` show a small design without a debugger.

## A wrong netlist: finding the pass

1. `lfc design.sv --verify` reports `MISMATCH` with the first differing
   output and, for small designs, the exact input assignment.
2. `--verify-each` runs the structural checker after every pass. If it throws,
   the message names the broken invariant and the pass that ran last.
3. If the IR is well-formed but the function is wrong, bisect with `--skip`:
   ```bash
   lfc design.sv --verify --skip sweep
   lfc design.sv --verify --skip balance,sweep
   ```
   The pass whose removal makes `--verify` pass is the suspect.
4. Shrink the input, add it to `tests/designs/`, and run `qor_regress --update`
   so it stays covered. The fuzzer prints the full source of any failing seed,
   so that part is usually already done.

## A worse netlist: QoR regressions

- `ctest` includes `qor`, which compares gates/area/depth/flops for every
  test design with `tests/golden/qor.txt`. Output is deterministic, so any
  difference is real. Improvements fail too, until someone runs
  `build/release/tests/qor_regress tests/designs tests/golden/qor.txt --update`
  on purpose and commits the new golden file with the change.
- For a bigger design, compare two runs pass by pass:
  ```bash
  lfc big.sv --json before.json          # old build
  lfc big.sv --json after.json           # new build
  python3 tools/qor_diff.py before.json after.json
  ```
  It exits 1 if QoR got worse and marks any pass that now removes less logic,
  which usually points straight at the change responsible.

## Slower: performance regressions

- `--profile` prints wall time per stage (parse, elaborate, partition,
  regions, merge, cleanup, write) and CPU time per pass summed over threads.
- The same output shows how many regions each worker ran and stole. One worker
  doing most of the work means imbalance (a giant region). Pass CPU time that
  grows with `-j` while the work stays the same means threads are waiting on
  something shared.
- `/usr/bin/time -v lfc ...` gives page faults and context switches. That's
  how the malloc problem in [performance.md](performance.md) was found.
- `tools/bench.py` for a proper scaling table (medians, determinism check).

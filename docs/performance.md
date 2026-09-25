# Performance

## Setup

- Intel Core Ultra 9 185H: 16 cores (6 performance, 8 efficient, 2 low-power
  efficient), 22 hardware threads
- Ubuntu 24.04 container on Docker Desktop / WSL2, GCC 13.3, `Release` (-O3)
- `python3 tools/bench.py --design <d> --reps 7 --threads 1,2,4,8,12,16,22`
  runs `lfc -j N -o netlist.v --json` 7 times per point and reports **medians**.
  Every run's netlist is hashed, and the script aborts if any thread count
  produces a different netlist.
- "total" is the whole run: parse, elaborate, optimize, write the netlist.
  "optimize" is the stage that's parallel: partition, regions, merge, cleanup.

Designs come from `tools/gen_bench.py` (independent datapath blocks with
constant selects, cancelling masks and dead logic mixed in):

| design | command | elaborated gates | optimized | regions |
|---|---|---:|---:|---:|
| b2000 | `--blocks 2000` | 836,371 | 166,781 | 6,059 |
| b4000 | `--blocks 4000` | 1,689,384 | 335,806 | 11,704 |
| b1000w16 | `--blocks 1000 --width 16` | 1,376,962 | 275,910 | 4,689 |

## Results

**b2000**

| threads | total ms | speedup | optimize ms | speedup |
|---:|---:|---:|---:|---:|
| 1 | 328.8 | 1.00x | 214.9 | 1.00x |
| 2 | 228.7 | 1.44x | 126.9 | 1.69x |
| 4 | 185.3 | 1.77x | 88.0 | 2.44x |
| 8 | 175.6 | 1.87x | 70.0 | 3.07x |
| 12 | 174.8 | 1.88x | 66.2 | 3.25x |
| 16 | 159.1 | 2.07x | 62.0 | 3.47x |
| 22 | 155.3 | 2.12x | 60.2 | 3.57x |

**b4000**: 16 threads: total 1.79x (561 -> 313 ms), optimize 3.24x (368 -> 114 ms)

**b1000w16**: 16 threads: total 1.91x (340 -> 178 ms), optimize 3.02x (251 -> 83 ms)

Full tables: [bench/](bench). Numbers wander by roughly ±10% between sessions
on a laptop, which is why the medians matter.

## Where the time goes

`lfc bench.sv --profile -o out.v` on b2000:

| stage | -j1 ms | -j16 ms | parallel? |
|---|---:|---:|---|
| parse | 40.7 | 32.0 | no |
| elaborate | 35.7 | 31.2 | no |
| partition | 16.4 | 17.1 | no |
| **regions** | **147.5** | **21.0** | yes, 7.0x |
| merge | 20.0 | 15.9 | no |
| cleanup (global dce) | 6.6 | 7.1 | no |
| write | 24.3 | 25.9 | no |
| total | 300.5 | 159.0 | |

The region phase scales about 7x on 16 threads. Everything around it is
serial. At `-j1` the serial parts are about half the run (~150 of ~300 ms), so
Amdahl's law caps the whole run at about 2x no matter how many cores, and the
measured 2.07x is right at that ceiling. Inside the optimize stage the serial
share is about 20%, which caps it near 4x at 16 threads; measured 3.0-3.5x.

The next wins are in the serial parts, not in more threads: parsing several
files/modules in parallel, a merge that writes regions into place from prefix
sums, a lexer that doesn't copy token text.

## Profiling notes

These are the changes that came out of profiling, in order, with what they
did to the numbers. They're kept because each one is a mistake that's easy to
make again.

**1. `std::unordered_map` everywhere.** The first version used
`unordered_map` for the strash table and the sweep's signature buckets, and a
`vector<vector<uint32_t>>` for support sets. That's one heap allocation per
gate, several times per pass. It measured 466 ms at `-j1`, with strash and
sweep the two most expensive passes. Replacements: an open-addressing strash
table that stores only node ids (keys are read back from the graph), a flat
`K`-slots-per-node support array, and a flat signature table. `-j1` went to
293 ms and the region phase from 301 to 159 ms. QoR was bit-for-bit the same,
which the golden test confirms.

**2. `-j16` slower than `-j8`.** To avoid a hash map when extracting regions
I gave each worker thread a dense scratch array sized to the *whole* design
(about 6.7 MB, zero-filled). With 16 threads that's over 100 MB of page faults
per run, and median total time at `-j16` became *worse* than `-j8` (276 vs
230 ms). Fix: `partition()` already knows each gate's index inside its region,
so it records it once (`pos[]`) and extraction needs no per-thread table.

**3. malloc arenas and the mmap lock.** Still poor scaling. `/usr/bin/time -v`
showed the difference:

| | -j1 | -j16 before | -j16 after |
|---|---:|---:|---:|
| minor page faults | 16,604 | 16,667 | 752 |
| voluntary context switches | 0 | 1,525 | 102 |
| region phase ms | ~170 | ~47 | ~22 |

Voluntary context switches mean threads were *blocking*. Every pass allocates
a new graph and frees the old one. glibc gives each thread its own arena, and
arenas grow and shrink their heaps with `mprotect`/`madvise`, which take the
process-wide mmap lock. So the threads were queueing on a kernel lock.
`main()` now calls `mallopt()` so memory isn't handed back mid-run (trim
threshold 1 GB, top pad 64 MB). It's a one-process, one-shot tool, so holding
on to freed memory costs nothing that matters.

**4. The netlist writer.** It looked up every fanin's name in a `std::map`.
It now uses a vector indexed by node id and appends to one reserved string.
Writing went from roughly 60 ms (estimated from runs with and without `-o`)
to 25 ms.

**5. A non-win.** The expression parser first recursed through 10 precedence
levels, each with a `std::set<std::string>` lookup, and I expected that to be
slow. After switching to precedence climbing (one lookup per operator), parse
time stayed about the same (~40 ms). The cost is in tokenizing and AST
allocation, not precedence. The change stayed because the code is shorter,
but it's a reminder to profile before optimizing.

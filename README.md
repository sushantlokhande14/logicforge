# LogicForge

A small parallel RTL compiler in C++17. `lfc` reads a synthesizable subset of
Verilog/SystemVerilog, bit-blasts it into a gate graph, optimizes it
(constant propagation, Boolean simplification, structural hashing, dead logic
removal, depth balancing, simulation-based sweeping) and writes a gate-level
Verilog netlist.

```bash
cmake --preset release
cmake --build build/release -j
ctest --test-dir build/release
./build/release/lfc examples/tiny.sv --stats --verify -o tiny_opt.v
```

# Supported language subset

`lfc` accepts the synthesizable core of Verilog-2001 and SystemVerilog. It
compiles one module (`--top NAME`, or the last one in the files).
[`examples/language_tour.sv`](../examples/language_tour.sv) uses most of it
and compiles:

```systemverilog
module m #(parameter W = 8) (          // ANSI header with parameters
  input  logic [W-1:0] a, b,           // logic / wire / reg; ranges from constants
  input  logic [1:0]   sel,
  input  logic         clk, rst,
  output logic [W:0]   sum,
  output logic [W-1:0] big, z,
  output logic [3:0]   q
);
  localparam STEP = W - 5;             // integer constant expressions: + - * << >>
  wire [3:0] nib = a[3:0];             // declaration with assignment

  assign sum = a + b;                  // continuous assignment, carry kept by context width

  always_comb begin                    // also: always @(*), always @*
    big = b;
    if (a > b) big = a;
  end

  always_comb
    case (sel)                         // unique/priority are accepted and ignored
      2'd0, 2'd1: z = a ^ {W{nib[0]}};
      default:    z = b << 1;
    endcase

  always_ff @(posedge clk)             // also: always @(posedge clk)
    if (rst) q <= 4'd0;
    else     q <= q + STEP;
endmodule
```

Old-style (non-ANSI) headers work too: `module m(a, b, y); input [3:0] a; ...`
(see `tests/designs/mux4.sv`).

**Operators:** `~ ! & | ^ ~^ ~& ~|` (unary and binary), `&& ||`,
`== != < <= > >=`, `+ - *`, `<< >>` by a constant, `?:`, `{a, b}`, `{n{a}}`,
constant bit-select `a[3]` and part-select `a[7:4]` on nets.

**Literals:** `8'hFF`, `4'b10_10`, `'o17`, `12'd100`, plain decimals.

## Rules worth knowing

- Everything is unsigned. `signed` is accepted and ignored.
- Parameters are 32-bit integers, whatever type they're declared with. You
  can't bit-select a parameter (`K[3:0]`).
- An `always_comb` variable must be assigned on every path. A `case` without
  `default` counts as incomplete even if it lists every value, because there's
  no coverage analysis, so add a `default`.
- Use `=` in `always_comb` and `<=` in `always_ff`. Mixing them is an error.
- Loop detection works per statement. A single `assign` whose right side reads
  its own left side (a carry chain written as one vector assign) is reported as
  a loop even if no individual bit is. Split it into per-bit assigns.

## Not supported (clear errors)

Module instances, `generate`, `function`/`task`, `initial`, `inout`,
`casez`/`casex`, `x`/`z` literals, asynchronous resets
(`@(posedge clk or negedge rst_n)`), variable indices (`a[i]`), `'0`/`'1` fill
literals, `/` and `%`.

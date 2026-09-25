# Walkthrough: one small design through every stage

`examples/tiny.sv`:

```systemverilog
module tiny (
  input  logic [1:0] a,
  input  logic [1:0] b,
  input  logic       en,
  output logic [1:0] y,
  output logic       any
);
  localparam FORCE = 0;
  logic [1:0] t;

  assign t   = (a & b) | (a & ~b);           // really just a
  assign y   = FORCE ? 2'b11 : (en ? t : 2'b00);
  assign any = |{y, 1'b0};
endmodule
```

A person sees at a glance that `y = en ? a : 0`, i.e. `y = a & {2{en}}`, and
`any = y[0] | y[1]`. The compiler has to earn that.

## 1. Parse

The lexer turns text into tokens (`assign`, `t`, `=`, `(`, `a`, `&`, ...) and
the parser builds a tree. For `(a & b) | (a & ~b)`:

```
        |
      /   \
     &     &
    / \   / \
   a   b a   ~
             |
             b
```

`&` binds tighter than `|`, so the parser needs operator precedence. It uses
precedence climbing: `binary(min_prec)` parses an operand, then keeps absorbing
operators whose precedence is at least `min_prec` (see `Parser::binary`).

## 2. Elaborate

Elaboration turns the tree into single-bit gates. `a` is 2 bits wide, so
`a & b` becomes two AND gates, one per bit. Output of `lfc examples/tiny.sv -O0 --dump-ir`
(31 identical lines trimmed):

```
n2 = input a[0]      n3 = input a[1]      n4 = input b[0]
n5 = input b[1]      n6 = input en[0]
n7 = or n0 n0        <- FORCE is a 32-bit integer; "FORCE ? ..." needs
...                     |FORCE, so 31 ORs of constant 0 (n0 is const0)
n37 = or n36 n0
n38 = and n2 n4      <- a & b, bit 0
n39 = and n3 n5      <- a & b, bit 1
n40 = not n4         <- ~b
n41 = not n5
n42 = and n2 n40     <- a & ~b
n43 = and n3 n41
n44 = or n38 n42     <- t[0]
n45 = or n39 n43     <- t[1]
n46 = mux n6 n44 n0  <- en ? t : 0
n47 = mux n6 n45 n0
n48 = mux n37 n1 n46 <- FORCE ? 1 : ...
n49 = mux n37 n1 n47
n50 = or n0 n48      <- |{y, 1'b0}
n51 = or n50 n49
```

45 gates, depth 34. The elaborator is deliberately dumb. It never folds or
shares anything, so each later improvement is visible and belongs to exactly
one pass.

Every node only reads lower-numbered nodes. That makes the node array a
topological order for free, and every pass is one loop from front to back.

## 3. constprop: 45 -> 11 gates, depth 34 -> 4

Constant folding with rules like `x | 0 = x`, `mux(0, a, b) = b`,
`mux(s, t, 0) = s & t`:

- the 31-OR chain is `0 | 0 | ... | 0`, which is `0`
- so `FORCE ? 2'b11 : ...` becomes a mux with a constant select, and folds away
- `en ? t : 0` becomes `en & t`
- `0 | y[0]` becomes `y[0]`

```
n7  = and a0 b0      n11 = and a0 ~b0     n13 = or n7 n11     (t[0])
n8  = and a1 b1      n12 = and a1 ~b1     n14 = or n8 n12     (t[1])
n15 = and en n13     n16 = and en n14     n17 = or n15 n16
```

## 4. simplify, strash, dce, balance: no change here

`simplify` knows identities like `x & ~x = 0`, but `(a&b) | (a&~b)` needs
distributivity, which none of its local rules cover. There are no duplicate
gates for `strash`, no dead logic for `dce`, and no long AND/OR chains for
`balance`.

## 5. sweep: t[i] is a[i]

`sweep` simulates every node on 256 random input patterns. `n13`
(`(a0&b0) | (a0&~b0)`) produces exactly the same 256 bits as the input `a[0]`,
so they're candidates. Random agreement isn't proof, so `sweep` then
enumerates **all** assignments of the inputs `n13` depends on (`a[0]`, `b[0]`:
4 patterns). They match, so every reader of `n13` now reads `a[0]` instead.

## 6. strash + dce: 3 gates

The old `t` logic now has no readers and `dce` deletes it:

```
n7 = and a[0] en
n8 = and a[1] en
n9 = or n7 n8
y[0] <- n7    y[1] <- n8    any <- n9
```

`--verify` then checks all 2^5 = 32 input combinations against the elaborated
graph: `verify: equivalent (exhaustive, 32 patterns)`.

## 7. Write

```verilog
module tiny (
  input [1:0] a,
  input [1:0] b,
  input en,
  output [1:0] y,
  output any
);
  wire _n7 = a[0] & en;
  wire _n8 = a[1] & en;
  wire _n9 = _n7 | _n8;
  assign y[0] = _n7;
  assign y[1] = _n8;
  assign any = _n9;
endmodule
```

This is valid Verilog that `lfc` can read back. The test suite does exactly
that for every design and checks the result is still equivalent.

To see the graph: `lfc examples/tiny.sv --dot tiny.dot && dot -Tpng tiny.dot -o tiny.png`.

// Deliberately wasteful logic. Almost all of it should disappear.
module redundant #(parameter MODE = 2) (
  input  logic [3:0] a, b,
  input  logic       s,
  output logic [3:0] y0, y1, y2,
  output logic       y3
);
  localparam logic [3:0] MASK = 4'hF;
  logic [3:0] unused_sum;
  logic [7:0] unused_prod;

  assign y0 = (a & b) | (a & ~b);                   // just a
  assign y1 = s ? (a ^ b) : (a ^ b);                // both arms equal
  assign y2 = (MODE == 2) ? (a & MASK) : (a * b);   // constant select, multiplier is dead
  assign y3 = &{a[0], a[0], ~a[0] | a[0]} ^ (b[1] & ~b[1]);  // a[0]
  assign unused_sum = a + b;                        // nobody reads these
  assign unused_prod = a * b;
endmodule

// p and q are the same function written two ways
module mac (
  input  logic [3:0] a, b, c,
  output logic [7:0] p, q
);
  assign p = a * b + c;
  assign q = c + b * a;
endmodule

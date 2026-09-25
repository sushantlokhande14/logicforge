module parity16 (
  input  logic [15:0] d,
  output logic        odd,
  output logic        even,
  output logic        all_ones,
  output logic        any
);
  assign odd = ^d;   // elaborates to a 15-deep xor chain
  assign even = ~^d;
  assign all_ones = &d;
  assign any = |d;
endmodule

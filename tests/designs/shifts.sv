module shifts (
  input  logic [7:0]  x,
  input  logic        dir,
  output logic [7:0]  y,
  output logic [15:0] wide
);
  assign y = dir ? (x << 2) : (x >> 3);
  assign wide = {{8{x[7]}}, x};  // sign extend
endmodule

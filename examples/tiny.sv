// The example used in docs/walkthrough.md
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

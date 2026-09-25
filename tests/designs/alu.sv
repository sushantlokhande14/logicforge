module alu4 (
  input  logic [3:0] a, b,
  input  logic [2:0] op,
  output logic [3:0] y,
  output logic       zero,
  output logic       carry
);
  logic [4:0] wide;

  always_comb begin
    wide = 5'd0;
    case (op)
      3'd0: wide = a + b;
      3'd1: wide = a - b;
      3'd2: wide = {1'b0, a & b};
      3'd3: wide = {1'b0, a | b};
      3'd4: wide = {1'b0, a ^ b};
      3'd5: wide = {1'b0, ~a};
      3'd6: wide = {4'd0, a < b};
      default: wide = {1'b0, a << 1};
    endcase
  end

  assign y = wide[3:0];
  assign carry = wide[4];
  assign zero = (y == 4'd0);
endmodule

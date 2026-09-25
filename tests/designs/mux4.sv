// old-style (Verilog-2001, non-ANSI) ports
module mux4 (sel, d0, d1, d2, d3, y);
  input [1:0] sel;
  input [2:0] d0, d1, d2, d3;
  output [2:0] y;
  reg [2:0] y;

  always @(*) begin
    case (sel)
      2'd0: y = d0;
      2'd1: y = d1;
      2'd2: y = d2;
      default: y = d3;
    endcase
  end
endmodule

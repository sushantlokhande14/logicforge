// loadable up-counter with synchronous reset
module counter (
  input  logic       clk,
  input  logic       rst,
  input  logic       en,
  input  logic       load,
  input  logic [3:0] d,
  output logic [3:0] q,
  output logic       wrap
);
  always_ff @(posedge clk) begin
    if (rst) q <= 4'd0;
    else if (load) q <= d;
    else if (en) q <= q + 4'd1;
  end

  assign wrap = en & (q == 4'hF);
endmodule

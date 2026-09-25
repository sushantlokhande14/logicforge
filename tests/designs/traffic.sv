module traffic (
  input  logic       clk,
  input  logic       rst,
  input  logic       car,
  output logic [1:0] light
);
  localparam GREEN = 2'd0, YELLOW = 2'd1, RED = 2'd2;
  logic [1:0] state, next;
  logic [2:0] timer;

  always_comb begin
    next = state;
    case (state)
      GREEN:  if (car && timer == 3'd7) next = YELLOW;
      YELLOW: next = RED;
      RED:    if (timer == 3'd3) next = GREEN;
      default: next = GREEN;
    endcase
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= GREEN;
      timer <= 3'd0;
    end else begin
      state <= next;
      timer <= (next != state) ? 3'd0 : timer + 3'd1;
    end
  end

  assign light = state;
endmodule

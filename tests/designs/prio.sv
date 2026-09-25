module prio8 (
  input  logic [7:0] req,
  output logic [2:0] idx,
  output logic       valid
);
  always_comb begin
    idx = 3'd0;
    if (req[7]) idx = 3'd7;
    else if (req[6]) idx = 3'd6;
    else if (req[5]) idx = 3'd5;
    else if (req[4]) idx = 3'd4;
    else if (req[3]) idx = 3'd3;
    else if (req[2]) idx = 3'd2;
    else if (req[1]) idx = 3'd1;
  end
  assign valid = |req;
endmodule

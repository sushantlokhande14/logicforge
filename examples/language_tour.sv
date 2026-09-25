module m #(parameter W = 8) (          // ANSI header with parameters
  input  logic [W-1:0] a, b,           // logic / wire / reg; ranges from constants
  input  logic [1:0]   sel,
  input  logic         clk, rst,
  output logic [W:0]   sum,
  output logic [W-1:0] big, z,
  output logic [3:0]   q
);
  localparam STEP = W - 5;             // integer constant expressions: + - * << >>
  wire [3:0] nib = a[3:0];             // declaration with assignment

  assign sum = a + b;                  // continuous assignment, carry kept by context width

  always_comb begin                    // also: always @(*), always @*
    big = b;
    if (a > b) big = a;
  end

  always_comb
    case (sel)                         // unique/priority are accepted and ignored
      2'd0, 2'd1: z = a ^ {W{nib[0]}};
      default:    z = b << 1;
    endcase

  always_ff @(posedge clk)             // also: always @(posedge clk)
    if (rst) q <= 4'd0;
    else     q <= q + STEP;
endmodule

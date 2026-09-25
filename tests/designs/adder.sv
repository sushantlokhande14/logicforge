// 4-bit adder with carry in and carry out
module adder4 (
  input  logic [3:0] a,
  input  logic [3:0] b,
  input  logic       cin,
  output logic [3:0] sum,
  output logic       cout
);
  assign {cout, sum} = a + b + cin;
endmodule

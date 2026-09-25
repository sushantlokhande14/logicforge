// binary -> gray -> binary. `back` is the same function as `bin`,
// which only the sweep pass can see.
module gray (
  input  logic [5:0] bin,
  output logic [5:0] gray,
  output logic [5:0] back
);
  assign gray = bin ^ (bin >> 1);
  assign back[5] = gray[5];
  assign back[4] = back[5] ^ gray[4];
  assign back[3] = back[4] ^ gray[3];
  assign back[2] = back[3] ^ gray[2];
  assign back[1] = back[2] ^ gray[1];
  assign back[0] = back[1] ^ gray[0];
endmodule

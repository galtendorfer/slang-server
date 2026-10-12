module SymResTopA;
    logic a, b, c;
    SymResLeaf first(.a(a), .y(b));
    SymResLeaf second(.a(b), .y(c));
endmodule

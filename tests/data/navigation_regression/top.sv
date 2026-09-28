module NavigationRegressionTop;
    logic a, b, c;
    NavigationRegressionALU first(.a(a), .y(b));
    NavigationRegressionALU second(.a(b), .y(c));
endmodule

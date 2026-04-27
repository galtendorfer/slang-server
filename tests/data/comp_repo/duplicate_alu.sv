module alu #(
    parameter int WIDTH = 32
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic [5:0]       op,
    output logic [WIDTH-1:0] result,
    output logic             zero,
    output logic             overflow
);

    assign result = '1;
    assign zero = 1'b0;
    assign overflow = 1'b0;

endmodule
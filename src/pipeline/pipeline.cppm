// The compilation pipeline of one translation unit (#43), run in-process by the c++-compatible
// front (`ycxx cxx`): source text through the front end, function_ir, lower and code generation to
// an object, then the link with libSystem to an executable; and the line table that diagnostics
// are rendered with.
export module ycxx.pipeline;

export import :lines;
export import :mangle;
export import :back;
export import :front;
export import :executable;

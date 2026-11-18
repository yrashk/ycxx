// The compilation pipeline (#43, #63) as engine queries, run by the c++-compatible front (`ycxx
// cxx`) through a `Session`: source text through the front end, function_ir, lower and code
// generation to an object (`tu(TU)`), then the link with libSystem to an executable; and the line
// table that diagnostics are rendered with.
export module ycxx.pipeline;

export import :lines;
export import :mangle;
export import :back;
export import :front;
export import :executable;
export import :session;

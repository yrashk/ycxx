// IR generation (#24): `function_ir(F, ctx)` builds a function's L1 body (ycxx.ir) from its
// analysed AST (ycxx.ast), and its `function_debug` beside it.
export module ycxx.irgen;

export import :facts;
export import :gen;
export import :types;

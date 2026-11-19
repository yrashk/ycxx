// ycxx.lex: translation phases 1–3, 5 and 6 ([lex.phases]) as functions, for `pp` (ycxx.pp) to
// run as a query. The token API the parser reads is `Tok` (:tok) and the region of ycxx.pp.
export module ycxx.lex;

export import :tok;
export import :builtins;
export import :unicode_tables;
export import :unicode;
export import :diag;
export import :source;
export import :lexer;
export import :literal;

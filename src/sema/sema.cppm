// ycxx.sema: the semantic half of the front end (docs/design/ast.md §3.2): the actions the
// parser (ycxx.parse) calls as it parses, the diagnostics table, constant evaluation through the
// interpreter (:eval), and, until #33 lands, the stand-in type, constant and entity tables
// (:types).
export module ycxx.sema;

export import :types;
export import :diag;
export import :templates;
export import :actions;
export import :eval;

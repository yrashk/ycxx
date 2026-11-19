// #133, from the step's review: a deleted operator function chosen by overload resolution makes
// the expression ill-formed, not the built-in operator ([over.match.oper]/3, [dcl.fct.def.delete]).
enum E { a = 1, b = 2 };
E operator|(E, E) = delete;
int f() { return a | b; } //! error

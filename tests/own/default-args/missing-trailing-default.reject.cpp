// ι (#300): every parameter after one with a default argument needs a default, from this or an
// earlier declaration ([dcl.fct.default]/4).
int f(int a = 1, int b); //! error

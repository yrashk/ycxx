// ι (#300): a later declaration cannot give a default argument to a parameter that already has
// one, not even the same value ([dcl.fct.default]/4).
int f(int a = 1);
int f(int a = 1); //! error

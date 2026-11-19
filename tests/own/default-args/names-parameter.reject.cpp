// ι (#300): a default argument cannot use another parameter of the function
// ([dcl.fct.default]/9).
int f(int a, int b = a); //! error

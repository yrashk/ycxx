// ι (#300): a default argument is checked as the initializer of its parameter; an int other
// than a null pointer constant does not convert to `int*` ([dcl.fct.default]/5, [conv.ptr]).
int f(int* p = 1); //! error

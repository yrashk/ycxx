// #133: a unary operator takes one operand ([over.unary]).
struct S {};
int operator~(S, S); //! error

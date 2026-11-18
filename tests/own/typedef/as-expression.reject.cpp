// #133: a typedef-name is not an expression ([expr.prim.id]).
typedef int T;
int x = T; //! error

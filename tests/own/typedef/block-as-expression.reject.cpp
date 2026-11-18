// #133: a block-scope alias-declaration's name is not an expression ([expr.prim.id]).
void f() {
  using T = int;
  int x = T; //! error
}

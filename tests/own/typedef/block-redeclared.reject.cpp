// #133: a block-scope typedef-name and a variable of the same block ([basic.scope.block]).
void f() {
  typedef int T;
  int T; //! error
}

// #133, from the step's review: a block-scope typedef-name in a function body's outermost block
// may not take a parameter's name ([basic.scope.block]/2).
int f(int T) {
  typedef int T; //! error
  return 0;
}

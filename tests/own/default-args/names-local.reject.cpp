// ι (#300): a default argument of a block-scope function declaration cannot use a local variable
// of the enclosing function ([dcl.fct.default]/7).
int g(int x);
int main() {
  int n = 3;
  int g(int x = n); //! error
  return g(1);
}
int g(int x) { return x; }

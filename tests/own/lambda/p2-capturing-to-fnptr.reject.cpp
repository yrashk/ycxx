// θ step 2 (#280), spec-only: only a closure type with no lambda-capture has a conversion to pointer to
// function ([expr.prim.lambda.closure], [conv]).
int main() {
  int k = 1;
  int (*f)(int) = [k](int x) { return x + k; }; //! error
  return f(1);
}

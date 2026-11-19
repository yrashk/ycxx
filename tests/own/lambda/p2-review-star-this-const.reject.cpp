// θ step 2 (#280), from the review: in a lambda that captures `*this` and is not mutable, the
// copy is const, through implicit member access too ([expr.prim.lambda.closure]/12).
struct S {
  int v = 1;
  int f() {
    [*this] { v = 3; }(); //! error
    return v;
  }
};
int main() { return S{}.f(); }

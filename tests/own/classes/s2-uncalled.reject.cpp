// δ s2 (#86), from the step's review: a member function named without a call, as a statement
// ([expr.ref]).
struct S {
  int v;
  void f() {}
};
int main() {
  S s{1};
  s.f; //! error
  return 0;
}

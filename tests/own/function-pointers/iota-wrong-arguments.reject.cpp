// ι (#313): a call through a pointer or reference to function checks its arguments against the
// pointee's parameters ([expr.call], [dcl.fct], [conv]).
int f(int) { return 1; }
int f(long) { return 2; }

struct S {
  int v;
};

int main() {
  int (*p)(int) = f;
  int (&r)(long) = f;
  p(1, 2); //! error
  p(); //! error
  p("x"); //! error
  r(S{1}); //! error
  (*p)(nullptr); //! error
  return 0;
}

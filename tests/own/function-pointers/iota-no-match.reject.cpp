// ι (#313): an overload set with no member of the target type cannot initialize, be assigned to,
// be passed as or be returned as a pointer or reference to function ([over.over],
// [dcl.init.general], [expr.assign], [stmt.return]).
int f(int) { return 1; }
int f(long) { return 2; }

int take(int (*)(double));

int (*give())(char*) {
  return f; //! error
}

int main() {
  int (*p)(char*) = f; //! error
  int (&r)(short) = f; //! error
  long (*q)(int) = &f; //! error
  int (*t)(int, int) = nullptr;
  t = f; //! error
  take(f); //! error
  return 0;
}

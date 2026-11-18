// δ s3 (#86): ambiguous overload resolution: two candidates reached by conversions of the same
// rank, for a function and for a constructor ([over.match.best], [over.ics.rank]).
int f(long) { return 1; }
int f(unsigned) { return 2; }

struct S {
  S(long) {}
  S(unsigned long) {}
};

int main() {
  int a = f(1); //! error
  short s = 2;
  int b = f(s); //! error
  S x(3); //! error
  return a + b;
}

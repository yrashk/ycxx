// δ s3 (#86): constructors with parameters, a default constructor, mem-initializer lists,
// default member initializers combined with constructors, and the forms `S s(1, 2)`,
// `S s{1, 2}`, `S s = S(1, 2)`, `S s = {1, 2}` calling a constructor
// ([class.ctor], [class.base.init], [dcl.init.general], [dcl.init.list]).
// EXIT: 0
class Fraction {
  int num_;
  int den_ = 1;
  bool reduced_ = false;

public:
  Fraction() : num_(0) {}
  Fraction(int n) : num_(n) {}
  Fraction(int n, int d) : num_(n), den_(d), reduced_(true) {
    int a = n < 0 ? -n : n, b = d;
    while (b != 0) { int t = a % b; a = b; b = t; }
    if (a > 1) { num_ /= a; den_ /= a; }
  }
  int num() const { return num_; }
  int den() const { return den_; }
  bool reduced() const { return reduced_; }
};

struct Interval {
  int lo, hi;
  Interval(int a, int b) : lo(a < b ? a : b), hi(a < b ? b : a) {}
  int len() const { return hi - lo; }
};

int main() {
  Fraction zero;
  if (zero.num() != 0 || zero.den() != 1 || zero.reduced()) return 1;
  Fraction three(3);
  if (three.num() != 3 || three.den() != 1) return 2;
  Fraction half(6, 12);
  if (half.num() != 1 || half.den() != 2 || !half.reduced()) return 3;
  Fraction third{4, 12};
  if (third.num() != 1 || third.den() != 3) return 4;
  Fraction q = Fraction(10, 4);
  if (q.num() != 5 || q.den() != 2) return 5;
  Fraction r = {9, 6};
  if (r.num() != 3 || r.den() != 2) return 6;
  Fraction e{};
  if (e.num() != 0 || e.den() != 1) return 7;
  Fraction seven = 7;  // a converting constructor
  if (seven.num() != 7) return 8;

  Interval i(9, 2);
  if (i.lo != 2 || i.hi != 9 || i.len() != 7) return 9;
  Interval j = Interval(1, 4);
  if (j.len() != 3) return 10;
  Interval k{5, 5};
  if (k.len() != 0) return 11;
  return 0;
}

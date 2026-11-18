// ζ c2 (#88): inside a member function of a class template, an unqualified call whose argument
// has a dependent type is looked up twice: ordinary lookup at the template's definition, plus
// argument-dependent lookup at the point of instantiation. describe(const geo::Point&), declared
// after the template in the argument's namespace, is found by ADL; for an int member only the
// describe(long) visible at the definition is a candidate, although describe(int) is declared
// later; and a name not declared at all at the definition is fine when the call is dependent
// ([temp.res], [temp.dep.general], [temp.dep.candidate], [basic.lookup.argdep], [temp.point]).
// EXIT: 0
int describe(long) { return 1; }

template<class T>
struct Holder {
  T v;
  int call() const { return describe(v); }
  int call_this() const { return describe(this->v) * 2; }
  int other() const { return measure(v); }
};

namespace geo {
  struct Point {
    int x;
  };
  int describe(const Point& p) { return 10 + p.x; }
  int measure(const Point& p) { return 100 + p.x; }
}

int describe(int) { return 3; }

int main() {
  Holder<geo::Point> h{{2}};
  if (h.call() != 12) return 1;
  if (h.call_this() != 24) return 2;
  if (h.other() != 102) return 3;
  Holder<int> i{5};
  if (i.call() != 1) return 4;
  if (i.call_this() != 2) return 5;
  if (describe(5) != 3) return 6;
  return 0;
}

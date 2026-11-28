// Distinct base overload and member imports in one using-declarator list remain valid
// ([namespace.udecl], [basic.scope.scope], #1552).
struct B1 {
  char f(int) const;
};
struct B2 {
  short f(double) const;
};
struct D1 : B1, B2 {
  using B1::f, B2::f;
};

struct Conv {
  operator int() const;
  operator double() const;
};
struct D2 : Conv {
  using Conv::operator int, Conv::operator double;
};

struct Base {
  char f(int) const;
};
struct Mid : Base {
  short f(double) const;
};
struct D3 : Mid {
  using Base::f, Mid::f;
};

template<class T> struct SingleDep : T {
  using T::f, T::g;
};

struct PairMembers {
  char f(int) const;
  short g(double) const;
};

void check(D1 d1, D2 d2, D3 d3, SingleDep<PairMembers> sd) {
  static_assert(sizeof(d1.f(1)) == sizeof(char));
  static_assert(sizeof(d1.f(1.0)) == sizeof(short));
  (void)d2;
  static_assert(sizeof(d3.f(1)) == sizeof(char));
  static_assert(sizeof(d3.f(1.0)) == sizeof(short));
  static_assert(sizeof(sd.f(1)) == sizeof(char));
  static_assert(sizeof(sd.g(1.0)) == sizeof(short));
}

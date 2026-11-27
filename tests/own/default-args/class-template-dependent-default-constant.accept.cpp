// In a class template's definition, a call that takes a dependent default argument is
// value-dependent ([temp.dep.constexpr]): `if constexpr`, a constexpr variable and an array
// bound wait for the specialization ([temp.inst]/12, [dcl.fct.default]).
template<class T> struct S {
  static constexpr int f(int x = sizeof(T)) { return x; }
  static constexpr int g() {
    if constexpr (f() == 8) return 1;
    else return 2;
  }
  static constexpr int h() {
    constexpr int k = f();
    int a[f()] = {};
    return k + int(sizeof(a) / sizeof(a[0]));
  }
};
static_assert(S<long>::g() == 1 && S<char>::g() == 2);
static_assert(S<long>::h() == 16 && S<char>::h() == 2);

// A member of a class template calls a member template unqualified; the member template's
// default depends on the class template's parameter ([temp.mem], [temp.inst]/12,
// [class.mem.general]). A later member template's default may be used by an earlier
// member's default, instantiated with the call.
template<class U> struct S {
  template<class T> static constexpr int f(T, int x = sizeof(U)) { return x; }
  template<class T> constexpr int m(T, int x = sizeof(U) + 1) const { return x; }
  static constexpr int g() { return f(1); }
  constexpr int n() const { return m(1); }
  static constexpr int early(int x = later(1)) { return x; }
  template<class T> static constexpr int later(T, int y = sizeof(U) * 2) { return y; }
};
static_assert(S<long>::g() == 8 && S<char>::g() == 1);
static_assert(S<long>{}.n() == 9);
static_assert(S<int>::early() == 8);

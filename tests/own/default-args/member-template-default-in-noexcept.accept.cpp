// An ordinary class's exception specification takes an earlier member template's default
// argument ([except.spec], [dcl.fct.default], [temp.mem]).
struct S {
  template<class T> static constexpr int g(T, int y = 2) noexcept { return y; }
  static void h() noexcept(noexcept(g(1))) {}
};
static_assert(noexcept(S::h()));
static_assert(S::g(0) == 2);

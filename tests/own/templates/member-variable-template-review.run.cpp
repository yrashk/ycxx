// #340 review: a class template's specialization finds the end of an initializer whose template
// arguments have commas; a nested class finds its enclosing class's member template before a
// namespace's; a block-scope using-declaration hides the member; `object.v<args>` finds a base's;
// a prvalue object is discarded in a constant expression ([temp.inst]/3, [class.nest],
// [basic.lookup.unqual], [class.member.lookup], [expr.ref]).
// EXIT: 0
template <int A, int B> struct P { static constexpr int value = A + B; };
template <int A, int B> constexpr int f() { return A * B; }
template <class T> struct C {
  template <int N> static constexpr int v = P<N, 1>::value + f<N, 2>();
};
template <int K> constexpr int v = 100 + K;
struct S {
  template <int K> static constexpr int v = K;
  struct In {
    static int g() { return v<2>; }
  };
  int h() {
    using ::v;
    return v<1>;
  }
};
struct B { template <int K> static constexpr int pv = K; };
struct D : B {};
struct E { template <int K> static constexpr int w = K; };
static_assert(C<int>::v<3> == 10);
static_assert(E{}.w<5> == 5);
int main() {
  D d;
  if (S::In::g() != 2 || S{}.h() != 101) return 1;
  return d.pv<7> + D::pv<1> == 8 ? 0 : 2;
}

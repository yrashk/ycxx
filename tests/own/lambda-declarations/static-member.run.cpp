// A lambda in a static data member's initializer (#338): the closure type belongs to the member,
// is complete in the incomplete class, and the body sees the members declared before
// ([expr.prim.lambda.closure], [class.static.data]).
struct Plain {
  static constexpr int m = [] { return 0; }();
};
struct A {
  static constexpr int k = 3;
  static constexpr int sum = [](int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += k;
    return s;
  }(4);
  static constexpr auto f = [](int x) { return x + k; };
  static constexpr int from_f = f(1);
  static inline int inl = [] { return 5; }();
  static const int cnst = [] { return 6; }();
  static constexpr int a = [] { return 1; }(), b = [] { return 2; }();
  static constexpr const char* s = [] { return "ab"; }();
  static constexpr int twice = [] { return [] { return 7; }; }()();
 private:
  static constexpr int hidden = 9;
  static constexpr int seen = [] { return hidden; }();
 public:
  static constexpr int get() { return seen; }
};
struct Out {
  struct In {
    static constexpr int m = [] { return 5; }();
    static constexpr auto g = [](int x) { return x + m; };
  };
  static constexpr int r = In::m + In::g(1);
};
namespace {
struct Local {
  static constexpr auto f = [](int x) { return x + 1; };
};
}
static_assert(Plain::m == 0);
static_assert(A::sum == 12 && A::from_f == 4 && A::f(2) == 5 && A::cnst == 6 && A::a == 1 && A::b == 2);
static_assert(A::s[1] == 'b' && A::twice == 7 && A::get() == 9 && Out::r == 11);
int main() {
  int one = 1;
  int (*p)(int) = A::f;
  if (Plain::m != 0 || A::f(one) != 4 || p(one) != 4 || A::inl != 5) return 1;
  if (Out::In::g(one) != 6 || Local::f(one) != 2) return 2;
  return 0;
}

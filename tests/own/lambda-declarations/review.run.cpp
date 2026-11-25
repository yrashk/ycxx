// What the review of #338 found: lambdas in declarations combined with argument-dependent lookup,
// operators, folds, captures, function templates, later declarations and complete-class contexts.
template <class F>
constexpr int apply(F f) { return f(); }
template <class F, class T>
constexpr int apply1(F f, T t) { return f(t); }
// A closure type reaches argument-dependent lookup before its variable's type is deduced.
auto plus = +[] { return 6; };
auto applied = apply([] { return 9; });
struct A {
  static constexpr auto p = +[](int i) { return i; };
  static constexpr auto f = [] { return apply([] { return 7; }); };
};
template <class T>
struct At {
  static constexpr auto p = +[](T i) { return i; };
  static constexpr int v = apply([] { return int(sizeof(T)); });
  static constexpr int w = apply1([](T t) { return int(t + 1); }, T(2));
};
static_assert(At<int>::v == 4 && At<int>::w == 3);
// A closure type of a template's definition is dependent.
template <class T>
constexpr int vt = apply([] { return int(sizeof(T)); });
template <class T>
constexpr int vt1 = apply1([](T t) { return int(t * 2); }, T(4));
template <class T>
concept Four = apply([] { return int(sizeof(T)); }) == 4;
template <class T>
concept FourNested = [] { return apply([] { return int(sizeof(T)); }); }() == 4;
static_assert(FourNested<int> && !FourNested<char>);
static_assert(vt<int> == 4 && vt<char> == 1 && vt1<int> == 8 && Four<int> && !Four<char>);
// Folds in generic lambdas of a template's declarations.
template <class T>
constexpr int fold = [](auto... x) { return (x + ... + 0); }(T{1}, 2);
template <int... I>
struct Seq {};
template <class T>
constexpr int indices = []<int... I>(Seq<I...>) { return (I + ... + 0); }(T());
template <class T>
struct Folds {
  static constexpr int v = [](auto... x) { return (x + ... + 0); }(T{1}, 3);
};
template <class... T>
concept Two = requires { requires [](auto... x) { return (x + ... + 0); }(T{1}...) == 2; };
template <class... T>
int two(T...) { return 0; }
template <class... T>
  requires Two<T...>
int two(T...) { return 1; }
static_assert(fold<int> == 3 && indices<Seq<1, 2, 4>> == 7 && Folds<int>::v == 4);
// A capturing generic lambda in a generic lambda's body, instantiated in a template.
template <class T>
concept Capturing = [](auto z) { return [=](auto y) { return 1 == y + z; }(1); }(0);
template <class T>
constexpr bool capturing = [](auto z) { return [=](auto y) { return 1 == y + z; }(1); }(0);
template <class T>
int capturing_body(T t) { return [](auto z) { return [=](auto y) { return 1 == y + z; }(1); }(t); }
static_assert(Capturing<int> && capturing<int>);
// What a lambda calls may be declared after it.
constexpr auto later = [](auto x) { return x.f(); };
struct Q {
  constexpr bool f() const { return true; }
};
template <class T>
constexpr bool later_vt = [](T x) { return x.f(); }(T{});
static_assert(later(Q{}) && later_vt<Q>);
// A member's body is a complete-class context, where the lambda's body is instantiated.
struct Complete {
  static constexpr auto f = [](auto x) { return x->v; };
  int v = 3;
  int g() { return f(this); }
};
// The variable's identity is not the one predicted before its initializer.
int dynamic() { return 5; }
int (*const pointers[])(int) = {[](int x) { return x + 1; }};
const int numbers[] = {[] { return dynamic(); }()};
namespace N {
template <class F>
const F cid(F f) { return f; }
}
decltype(auto) kept = N::cid([] { return 9; });
extern "C" {
auto c_linkage = [](int x) { return x + 2; };
}
template <class T>
constexpr auto spec = [](T t) { return int(t); };
template <class T>
constexpr auto spec<T*> = [](T*) { return 30; };
template <>
constexpr auto spec<int> = [](int) { return 40; };
template <>
constexpr auto spec<long> = [](int) { return 50; };
static_assert(spec<char>(1) == 1 && spec<int>(1) == 40 && spec<long>(1) == 50);
int main() {
  int one = 1;
  if (plus() != 6 || applied != 9 || A::p(one) != 1 || A::f() != 7 || At<int>::p(one) != 1) return 1;
  if (two(1, 2) != 1 || two(1) != 0 || capturing_body(0) != 1) return 2;
  Complete c;
  if (c.g() != 3 || pointers[0](one) != 2 || numbers[0] != 5 || kept() != 9 || c_linkage(one) != 3) return 3;
  return spec<char*>(nullptr) == 30 && spec<int>(one) == 40 && spec<long>(one) == 50 ? 0 : 4;
}

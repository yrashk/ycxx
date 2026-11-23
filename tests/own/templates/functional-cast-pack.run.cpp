// #915: a functional conversion whose arguments are pack expansions, `T(a...)` ([expr.type.conv],
// [temp.variadic]/5): with a class type, a scalar type from a one-element pack, an expansion
// beside other arguments, and through a member of a dependent class reached by an alias, as
// libycxx's `invoke` calls `__invoker_for<F, A...>::__call(…)`.
// EXIT: 0
struct P { int x, y, z; P(int a, int b, int c) : x(a), y(b), z(c) {} };
template<class T, class... A> T make(A... a) { return T(a...); }
template<class T, class... A> T make_last(A... a) { return T(a..., 30); }
template<class... A> int scalar(A... a) { return int(a...); }

enum class K { plain };
template<class F, class... A> consteval K classify() { return K::plain; }
template<K k> struct Invoker;
template<> struct Invoker<K::plain> { template<class F, class... A> static int call(F&& f, A&&... a) { return f(a...); } };
template<class F, class... A> using invoker_for = Invoker<classify<F, A...>()>;
template<class F, class... A> int invoke(F&& f, A&&... a) {
  return invoker_for<F, A...>::call(static_cast<decltype(f)&&>(f), static_cast<decltype(a)&&>(a)...);
}
int add(int x, int y) { return x + y; }

int main() {
  P p = make<P>(1, 2, 3);
  P q = make_last<P>(10, 20);
  if (p.x + p.y + p.z != 6 || q.z != 30 || q.x != 10) return 1;
  if (scalar(7) != 7) return 2;
  if (invoke(add, 4, 5) != 9) return 3;
  return 0;
}

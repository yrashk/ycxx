// #1400: an expansion nested in another operand, `h(g(args...))`, in a function body and in a
// mem-initializer ([temp.variadic]): the inner expansion takes `args`, so the enclosing operand
// names no pack. An instantiation reported `args` as used without '...'.
// EXIT: 0
int g(int a, int b) { return a * 10 + b; }
int h(int a) { return a + 1; }
struct Body { int v; template<class... A> Body(A... args) { v = h(g(args...)); } };
struct Init { int v; template<class... A> Init(A... args) : v(h(g(static_cast<A&&>(args)...))) {} };
template<class T> struct Member {
  T value;
  template<class... A> Member(int, A... args) : value(h(g(args...)), g(args...)) {}
};
struct Pair { int a, b; Pair(int x, int y) : a(x), b(y) {} };
int main() {
  if (Body(1, 2).v != 13) return 1;
  if (Init(3, 4).v != 35) return 2;
  Member<Pair> m(0, 5, 6);
  if (m.value.a != 57 || m.value.b != 56) return 3;
  return 0;
}

// #356: a default argument of a member function or constructor of a class template is a
// complete-class context, so it names members declared later in the class: a static data
// member, a static member function, an enumerator and a nested type ([class.mem.general],
// [basic.scope.class], [dcl.fct.default], [temp.inst]).
// EXIT: 0
template<class T>
struct Later {
  int v;
  Later(int x = base + limit()) : v(x) {}
  int data(int x = base) { return x; }
  int call(int x = limit()) { return x; }
  int enumerator(int x = two) { return x; }
  int size(int x = sizeof(Inner)) { return x; }
  T typed(T x = T(base) + T(two)) { return x; }

  static constexpr int base = sizeof(T) * 10;
  static int limit() { return 3; }
  enum { two = 2 };
  struct Inner { T c[5]; };
};

int main() {
  Later<char> c;
  if (c.v != 13) return 1;
  if (c.data() != 10) return 2;
  if (c.call() != 3) return 3;
  if (c.enumerator() != 2) return 4;
  if (c.size() != 5) return 5;
  if (c.typed() != 12) return 6;
  Later<int> i;
  if (i.v != 43) return 7;
  if (i.size() != 20) return 8;
  if (i.typed() != 42 || i.typed(5) != 5) return 9;
  return 0;
}

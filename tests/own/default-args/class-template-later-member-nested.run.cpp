// #356: the complete-class context of a default argument also holds in a nested class of a
// class template and in a partial specialization ([class.mem.general], [class.nest], [temp.spec.partial], [dcl.fct.default]).
// EXIT: 0
template<class T>
struct Outer {
  struct Nested {
    int get(int x = own + 100) { return x; }
    int size(int x = sizeof(Deep)) { return x; }
    static constexpr int own = 1;
    struct Deep { T c[2]; };
  };
};

template<class T, class U>
struct Pair {
  int get(int x = 1) { return x; }
};

template<class T>
struct Pair<T, T*> {
  Pair(int x = scale * 2) : v(x) {}
  int get(int x = scale + tail()) { return x; }
  int v;
  static constexpr int scale = sizeof(T);
  static int tail() { return 50; }
};

int main() {
  Outer<int>::Nested n;
  if (n.get() != 101) return 1;
  if (n.size() != 2 * static_cast<int>(sizeof(int))) return 2;
  if (Pair<char, int>{}.get() != 1) return 3;
  Pair<short, short*> p;
  if (p.v != 2 * static_cast<int>(sizeof(short))) return 4;
  if (p.get() != static_cast<int>(sizeof(short)) + 50) return 5;
  if (p.get(9) != 9) return 6;
  return 0;
}

// ι (#300): member functions and a constructor of a class template with default arguments that
// depend on the template's parameters, instantiated per specialization
// ([dcl.fct.default]/1, [temp.inst], [class.default.ctor]/1).
// EXIT: 0
template<class T, int N = 4>
struct Box {
  T v;
  Box(T v = T(1)) : v(v) {}
  T get(T fallback = T(7)) const { return v ? v : fallback; }
  static int size(int extra = N) { return static_cast<int>(sizeof(T)) + extra; }
  T add(T d = T(N)) { v = v + d; return v; }
};

int main() {
  Box<int> a;
  if (a.v != 1) return 1;
  if (a.get() != 1) return 2;
  a.v = 0;
  if (a.get() != 7 || a.get(3) != 3) return 3;
  if (Box<int>::size() != static_cast<int>(sizeof(int)) + 4) return 4;
  if (Box<char, 10>::size() != 11) return 5;
  Box<long, 20> b(2);
  if (b.add() != 22L) return 6;
  if (b.add(1) != 23L) return 7;
  Box<char> c;
  if (c.get() != 1 || c.add() != 5) return 8;
  return 0;
}

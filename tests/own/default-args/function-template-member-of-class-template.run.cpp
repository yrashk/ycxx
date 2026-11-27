// #356: member function templates of a class template whose default arguments depend on the
// class template's parameter, on the member template's own parameter, or on both: a member
// function, a constructor template, a static member function template, explicit template
// arguments, and a default naming a later member ([temp.mem], [class.mem.general],
// [dcl.fct.default], [temp.inst]).
// EXIT: 0
template<class T>
struct Box {
  int v;
  template<class U> Box(U, int x = sizeof(T) * 10 + sizeof(U)) : v(x) {}
  template<class U> int outer(int x = sizeof(T)) { return x + static_cast<int>(sizeof(U)) * 100; }
  template<class U> int inner(int x = sizeof(U)) { return x; }
  template<class U> int both(int x = sizeof(T) * 10 + sizeof(U)) { return x; }
  template<class U> static int s(int x = sizeof(T) + sizeof(U) + extra()) { return x; }
  template<class U> T later(T t = T(scale) * T(sizeof(U))) { return t; }

  static int extra() { return 1000; }
  static constexpr int scale = 3;
};

int main() {
  Box<char> b(0);
  if (b.v != 10 + static_cast<int>(sizeof(int))) return 1;
  Box<int> c('x', 7);
  if (c.v != 7) return 2;
  if (b.outer<short>() != 1 + 100 * static_cast<int>(sizeof(short))) return 3;
  if (b.inner<char>(2) != 2) return 4;
  if (b.inner<long>() != static_cast<int>(sizeof(long))) return 5;
  if (b.both<char>() != 11) return 6;
  if (c.both<char>() != 10 * static_cast<int>(sizeof(int)) + 1) return 7;
  if (Box<short>::s<char>() != static_cast<int>(sizeof(short)) + 1001) return 8;
  if (Box<long>::s<int>(5) != 5) return 9;
  if (c.later<short>() != 3 * static_cast<int>(sizeof(short))) return 10;
  if (c.later<char>(4) != 4) return 11;
  return 0;
}

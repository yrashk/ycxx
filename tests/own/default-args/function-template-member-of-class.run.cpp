// #356: member function templates of an ordinary class whose default arguments depend on the
// member template's own parameter: a member function, a constructor template, a static member
// function template, an explicit template argument, and a default naming a later member of
// the class ([temp.mem], [class.mem.general], [dcl.fct.default], [temp.inst]).
// EXIT: 0
struct Plain {
  int v;
  template<class U> Plain(U, int x = sizeof(U) + bias) : v(x) {}
  template<class U> int f(int x = sizeof(U)) { return x; }
  template<class U> U g(U u, U d = U(later)) { return u + d; }
  template<class U> static int s(int x = sizeof(U) * 2) { return x; }
  template<class U> int deduced(U u, int x = sizeof(u) + 100) { return x; }

  static constexpr int bias = 30;
  static constexpr int later = 5;
};

int main() {
  Plain p('c');
  if (p.v != 31) return 1;
  Plain q(0L, 4);
  if (q.v != 4) return 2;
  if (p.f<char>() != 1) return 3;
  if (p.f<int>() != static_cast<int>(sizeof(int))) return 4;
  if (p.f<char>(2) != 2) return 5;
  if (p.g(1) != 6 || p.g(1, 2) != 3) return 6;
  if (p.g<long>(10) != 15L) return 7;
  if (Plain::s<short>() != 2 * static_cast<int>(sizeof(short))) return 8;
  if (p.s<char>(9) != 9) return 9;
  if (p.deduced(1.0) != 100 + static_cast<int>(sizeof(double))) return 10;
  return 0;
}

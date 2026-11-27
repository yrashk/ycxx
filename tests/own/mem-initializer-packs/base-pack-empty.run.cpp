// #1400: a base pack's mem-initializer pack expansion `Bs(x)...` with an empty pack expands to
// no mem-initializer at all ([class.base.init], [temp.variadic]), beside an ordinary member
// initializer.
// XFAIL: ycxx needs a pack expansion in a class template's base-specifier-list (#308)
// EXIT: 0
struct Counted { int v; Counted() : v(77) {} Counted(int x) : v(x) {} };
template<class... Bs> struct D : Bs... {
  int member;
  D(int x) : Bs(x)..., member(x) {}
};

int main() {
  D<> d(3);
  if (d.member != 3) return 1;
  D<Counted> one(4);
  if (one.v != 4 || one.member != 4) return 2;
  return 0;
}

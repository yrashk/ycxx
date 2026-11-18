// ζ c2 (#88), from the step's review: `typename T::a` and `typename T::b` are distinct
// types, so members overload on them, and a function template's parameter of such a type is a
// non-deduced context ([temp.res.general]/5, [temp.deduct.type]/5).
// EXIT: 0
struct P { enum a { x = 1 }; enum b { y = 2 }; enum type { z = 3 }; };
template<class T> struct B { int f(typename T::a) { return 1; } int f(typename T::b) { return 2; } };
template<class T> int g(typename T::type v) { return v; }
int main() {
  B<P> b;
  if (b.f(P::y) != 2) return 1;
  if (b.f(P::x) != 1) return 2;
  if (g<P>(P::z) != 3) return 3;
  return 0;
}

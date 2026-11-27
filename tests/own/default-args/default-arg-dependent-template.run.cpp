// ι (#300): a function template's default argument whose type and value depend on the template
// parameter. With one argument, T is deduced from it alone and the default `T(5)` is
// instantiated for that T; a default that would be ill-formed for some T is instantiated only
// when a call uses it ([dcl.fct.default]/1, [temp.deduct.call]/1, [temp.inst]).
// EXIT: 0
template<class T> T g(T x, T y = T(5)) { return x + y; }

struct Num {
  int v;
  Num(int v) : v(v) {}
};
Num operator+(Num a, Num b) { return Num(a.v + b.v); }

struct NoValue {};

template<class T> int pick(T, int n = T::value) { return n; }  // NoValue has no `value`

struct HasValue { static constexpr int value = 8; };

int main() {
  if (g(1) != 6) return 1;
  if (g(1, 2) != 3) return 2;
  if (g('a') != 'a' + 5) return 3;
  if (g(Num(10)).v != 15) return 4;
  if (g<long>(3) != 8L) return 5;
  if (pick(NoValue{}, 4) != 4) return 6;  // the default is not instantiated
  if (pick(HasValue{}) != 8) return 7;
  return 0;
}

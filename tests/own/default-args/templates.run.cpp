// ι (#300): a function template with a default argument of a non-dependent type, called with
// and without it. A parameter that gets no argument takes no part in deduction, and the default
// is used in each specialization ([dcl.fct.default]/1, [temp.deduct.call]/1, [temp.inst]).
// EXIT: 0
template<class T> T f(T x, int n = 3) { return x * n; }

template<class T> int count(T, int base = 10) { return base + static_cast<int>(sizeof(T)); }

template<class T, class U> T first(T a, U, int k = 1) { return a + k; }

int main() {
  if (f(2) != 6) return 1;
  if (f(2, 4) != 8) return 2;
  if (f(5L) != 15L) return 3;
  if (f<long>(1) != 3L) return 4;
  if (count('a') != 11) return 5;
  if (count('a', 0) != 1) return 6;
  if (first(1, 'x') != 2) return 7;
  if (first(1, 'x', 5) != 6) return 8;
  return 0;
}

// ζ f1 (#88): a function argument for a parameter of type T taken by value decays to a pointer
// to function, and T can be deduced from the parameter and return types of a pointer to function
// ([temp.deduct.call], [temp.deduct.type]).
// EXIT: 0
int square(int x) { return x * x; }
int negate(int x) { return -x; }
long twice(long x) { return x + x; }

template<class F>
int apply(F f, int x) { return f(x); }

template<class T>
T apply_ptr(T (*f)(T), T x) { return f(x); }

template<class F>
int apply_twice(F f, int x) {
  F g = f;
  return g(f(x));
}

int main() {
  if (apply(square, 5) != 25) return 1;
  if (apply(negate, 5) != -5) return 2;
  if (apply(&square, 3) != 9) return 3;
  if (apply_ptr(square, 4) != 16) return 4;
  if (apply_ptr(twice, 21L) != 42L) return 5;
  if (apply_twice(square, 2) != 16) return 6;
  if (apply_twice(negate, 7) != 7) return 7;
  return 0;
}

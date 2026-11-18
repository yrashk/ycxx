// ζ f1 (#88): templates calling templates: deduction inside a template body from a dependent
// argument, nested calls, an explicit prefix with the rest deduced from a dependent argument,
// and a template parameter passed on as an explicit argument ([temp.inst], [temp.deduct.call],
// [temp.arg.explicit]).
// EXIT: 0
template<class T>
T twice(T x) { return x + x; }

template<class T>
T quad(T x) { return twice(twice(x)); }

template<class T, class U>
T mix(T a, U b) { return twice(a) + quad(b); }

template<int N, class T>
T power(T x) {
  T r = 1;
  for (int i = 0; i < N; ++i) r = r * x;
  return r;
}

template<class T>
T cube(T x) { return power<3>(x); }

template<class T>
T cube_explicit(T x) { return power<3, T>(x); }

template<int N>
int power_of_two() { return power<N>(2); }

int main() {
  if (twice(4) != 8) return 1;
  if (quad(3) != 12) return 2;
  if (quad(2L) != 8L) return 3;
  if (mix(1, 2L) != 10) return 4;
  if (mix(1L, '\3') != 14L) return 5;
  if (cube(3) != 27) return 6;
  if (cube(-2L) != -8L) return 7;
  if (cube_explicit(4) != 64) return 8;
  if (power_of_two<10>() != 1024) return 9;
  if (power_of_two<0>() != 1) return 10;
  return 0;
}

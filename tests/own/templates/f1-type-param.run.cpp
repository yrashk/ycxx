// ζ f1 (#88): function templates at namespace scope with one type parameter, declared with
// `class` and with `typename`, deduced from a call and used with int, char, long and bool
// ([temp.pre], [temp.param], [temp.deduct.call]).
// EXIT: 0
template<class T>
T larger(T a, T b) {
  if (a < b) return b;
  return a;
}

template<typename T>
T clamp_to(T v, T lo, T hi) {
  T m = larger(lo, v);
  if (hi < m) return hi;
  return m;
}

template<class T>
bool same(T a, T b) { return a == b; }

int main() {
  if (larger(3, 7) != 7) return 1;
  if (larger(-2, -9) != -2) return 2;
  if (larger('a', 'z') != 'z') return 3;
  if (larger(100000L, 99999L) != 100000L) return 4;
  if (larger(false, true) != true) return 5;
  if (clamp_to(15, 0, 10) != 10) return 6;
  if (clamp_to(-5, 0, 10) != 0) return 7;
  if (clamp_to('m', 'a', 'f') != 'f') return 8;
  if (clamp_to(4, 0, 10) != 4) return 9;
  if (!same(3u, 3u)) return 10;
  if (same('x', 'y')) return 11;
  return 0;
}

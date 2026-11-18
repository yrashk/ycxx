// ζ f1 (#88): a function parameter whose type has no template parameter takes part in no
// deduction and accepts implicit conversions; a template parameter that appears in several
// function parameters is deduced from each, consistently ([temp.deduct.call],
// [temp.arg.explicit]).
// EXIT: 0
template<class T>
T scale(T x, int k) { return x * k; }

template<class T>
int count_eq(const T* a, int n, T v) {
  int c = 0;
  for (int i = 0; i < n; ++i)
    if (a[i] == v) ++c;
  return c;
}

template<class T>
T pick(bool first, T a, T b) {
  if (first) return a;
  return b;
}

int main() {
  if (scale(3L, 'c') != 297L) return 1;
  if (scale(2, true) != 2) return 2;
  if (scale('\2', 3L) != 6) return 3;
  int xs[5] = {1, 2, 1, 3, 1};
  if (count_eq(xs, 5L, 1) != 3) return 4;
  char cs[3] = {'a', 'b', 'a'};
  if (count_eq(cs, '\3', 'a') != 2) return 5;
  if (pick(1, 4, 5) != 4) return 6;
  if (pick(0, 'x', 'y') != 'y') return 7;
  return 0;
}

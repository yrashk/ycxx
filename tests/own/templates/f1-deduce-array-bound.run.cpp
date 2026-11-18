// ζ f1 (#88): non-type template parameters deduced from array bounds through a reference to
// array, one and two dimensions, with the element type deduced too, and used in the body
// ([temp.deduct.type], [temp.deduct.call]).
// EXIT: 0
template<int N>
int count(int (&)[N]) { return N; }

template<class T, int N>
T* end_of(T (&a)[N]) { return a + N; }

template<int R, int C>
int cells(int (&)[R][C]) { return R * 100 + C; }

template<class T, int N>
void fill(T (&a)[N], T v) {
  for (int i = 0; i < N; ++i) a[i] = v;
}

template<int N>
int sum(const int (&a)[N]) {
  int s = 0;
  for (int i = 0; i < N; ++i) s += a[i];
  return s;
}

int main() {
  int a[5] = {1, 2, 3, 4, 5};
  int b[1] = {9};
  int grid[2][3] = {{1, 2, 3}, {4, 5, 6}};
  char cs[4] = {'a', 'b', 'c', 'd'};
  if (count(a) != 5) return 1;
  if (count(b) != 1) return 2;
  if (end_of(a) != a + 5) return 3;
  if (end_of(cs) - cs != 4) return 4;
  if (cells(grid) != 203) return 5;
  if (count(grid[1]) != 3) return 6;
  fill(cs, 'z');
  if (cs[0] != 'z' || cs[3] != 'z') return 7;
  if (sum(a) != 15) return 8;
  fill(a, 2);
  if (sum(a) != 10) return 9;
  return 0;
}

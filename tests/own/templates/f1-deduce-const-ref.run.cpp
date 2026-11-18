// ζ f1 (#88): deduction for a parameter of type const T&: T = int from an int lvalue, a const
// int lvalue and an rvalue; no array-to-pointer conversion, so T and a non-type parameter are
// deduced from an array's element type and bound; the reference binds to the argument itself
// ([temp.deduct.call], [temp.deduct.type], [dcl.init.ref]).
// EXIT: 0
int id(int*) { return 1; }
int id(const int*) { return 2; }

struct Rect {
  int w;
  int h;
  int area() const { return w * h; }
};

template<class T>
int cref_kind(const T& r) {
  T x = r;
  x = x + 0;
  return id(&x) * 10 + id(&r);
}

template<class T>
const T& min_ref(const T& a, const T& b) {
  if (b < a) return b;
  return a;
}

template<class T, int N>
int length(const T (&)[N]) { return N; }

template<class T, int N>
T total(const T (&a)[N]) {
  T s = 0;
  for (int i = 0; i < N; ++i) s += a[i];
  return s;
}

template<class T>
int area_of(const T& s) { return s.area() + s.w; }

int main() {
  int i = 3;
  const int ci = 5;
  if (cref_kind(i) != 12) return 1;
  if (cref_kind(ci) != 12) return 2;
  if (cref_kind(7) != 12) return 3;
  if (cref_kind(i + 1) != 12) return 4;
  int j = 2;
  if (&min_ref(i, j) != &j) return 5;
  if (&min_ref(i, ci) != &i) return 6;
  if (min_ref(10, 20) != 10) return 7;
  int arr[4] = {1, 2, 3, 4};
  const char text[6] = {'h', 'e', 'l', 'l', 'o', 0};
  if (length(arr) != 4) return 8;
  if (length(text) != 6) return 9;
  if (total(arr) != 10) return 10;
  Rect r{3, 4};
  if (area_of(r) != 15) return 11;
  return 0;
}

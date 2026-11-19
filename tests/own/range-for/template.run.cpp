// ι (#298): range-based for inside function templates: the range's type is dependent, so the
// array or class form of begin/end is chosen per specialization ([stmt.ranged]/1,
// [temp.dep.expr]); one template instantiated for two array types and a class, another whose loop
// variable's type is a template parameter, and a class template with a range-for in a member.
// EXIT: 0
template <class R>
int sum(R& r) {
  int s = 0;
  for (auto x : r) s += x;
  return s;
}

template <class R>
void scale(R& r, int k) {
  for (auto& x : r) x *= k;
}

template <class T, int N>
T last(const T (&a)[N]) {
  T result = T();
  for (T x : a) result = x;
  return result;
}

struct Digits {
  int d[3];
  Digits() {
    d[0] = 1;
    d[1] = 2;
    d[2] = 4;
  }
  int* begin() { return d; }
  int* end() { return d + 3; }
};

template <class T, int N>
struct Array {
  T data[N];
  T* begin() { return data; }
  T* end() { return data + N; }
  T total() {
    T t = T();
    for (const T& x : *this) t += x;
    return t;
  }
};

int main() {
  int a4[4] = {1, 2, 3, 4};
  long a2[2] = {100, 200};
  Digits dg;
  if (sum(a4) != 10) return 1;
  if (sum(a2) != 300) return 2;
  if (sum(dg) != 7) return 3;

  scale(a4, 3);
  scale(dg, 10);
  if (a4[3] != 12 || dg.d[2] != 40) return 4;

  char cs[3] = {'p', 'q', 'r'};
  if (last(cs) != 'r') return 5;
  if (last(a2) != 200) return 6;

  Array<int, 3> arr;
  arr.data[0] = 5;
  arr.data[1] = 6;
  arr.data[2] = 7;
  if (arr.total() != 18) return 7;
  scale(arr, 2);
  if (sum(arr) != 36) return 8;
  return 0;
}

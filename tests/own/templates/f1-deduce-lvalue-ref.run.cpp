// ζ f1 (#88): deduction for a parameter of type T&: T = int from an int lvalue, T = const int
// from a const int lvalue, T = int[3] from an array (no decay), T = int* from a pointer; the
// reference refers to the caller's object ([temp.deduct.call], [temp.deduct.type]).
// EXIT: 0
int id(int*) { return 1; }
int id(const int*) { return 2; }
int id(int**) { return 3; }
int id(int (*)[3]) { return 4; }

struct Pair {
  int a;
  int b;
};

template<class T>
int ref_kind(T& r) { return id(&r); }

template<class T>
void twice(T& r) { r = r * 2; }

template<class T>
void swap_vals(T& a, T& b) {
  T t = a;
  a = b;
  b = t;
}

template<class T>
void reset(T& p) {
  p.a = 0;
  p.b = 0;
}

int main() {
  int i = 5;
  const int ci = 6;
  int arr[3] = {1, 2, 3};
  int* p = &i;
  if (ref_kind(i) != 1) return 1;
  if (ref_kind(ci) != 2) return 2;
  if (ref_kind(arr) != 4) return 3;
  if (ref_kind(p) != 3) return 4;
  twice(i);
  if (i != 10) return 5;
  twice(arr[1]);
  if (arr[1] != 4) return 6;
  int j = 1;
  swap_vals(i, j);
  if (i != 1 || j != 10) return 7;
  int* q = &j;
  swap_vals(p, q);
  if (p != &j || q != &i) return 8;
  Pair pr{4, 5};
  reset(pr);
  if (pr.a != 0 || pr.b != 0) return 9;
  return 0;
}

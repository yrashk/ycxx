// ζ f1 (#88): deduction for a parameter of type T taken by value: top-level cv-qualifiers of
// the argument are dropped, an array argument decays to a pointer, the parameter is a modifiable
// copy ([temp.deduct.call]). The deduced type is observed through overloads on a pointer to it.
// EXIT: 0
int id(int*) { return 1; }
int id(const int*) { return 2; }
int id(int**) { return 3; }
int id(const int**) { return 4; }
int id(char*) { return 5; }
int id(long*) { return 6; }
int id(bool*) { return 7; }

template<class T>
int kind(T x) { return id(&x); }

template<class T>
T bump(T x) {
  x = x + 1;
  return x;
}

int main() {
  int i = 1;
  const int ci = 4;
  int arr[3] = {1, 2, 3};
  const int carr[2] = {5, 6};
  int* const cp = &i;
  if (kind(i) != 1) return 1;
  if (kind(ci) != 1) return 2;
  if (kind(42) != 1) return 3;
  if (kind(arr) != 3) return 4;
  if (kind(carr) != 4) return 5;
  if (kind(cp) != 3) return 6;
  if (kind(&ci) != 4) return 7;
  if (kind('c') != 5) return 8;
  if (kind(5L) != 6) return 9;
  if (kind(true) != 7) return 10;
  if (bump(ci) != 5) return 11;
  if (ci != 4) return 12;
  if (bump(i) != 2 || i != 1) return 13;
  return 0;
}

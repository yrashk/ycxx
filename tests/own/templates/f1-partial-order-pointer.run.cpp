// ζ f1 (#88): partial ordering of function templates on pointer parameters: f(T*) is more
// specialized than f(T), f(const T*) than f(T*) for a pointer to const, f(T**) than f(T*); a
// better conversion sequence decides before partial ordering ([temp.over], [over.match.best],
// [temp.func.order], [temp.deduct.partial]).
// EXIT: 0
template<class T>
int f(T) { return 1; }
template<class T>
int f(T*) { return 2; }
template<class T>
int f(const T*) { return 3; }

template<class T>
int g(T*) { return 1; }
template<class T>
int g(T**) { return 2; }

struct S {
  int v;
};

int main() {
  int i = 0;
  const int ci = 0;
  int arr[3] = {1, 2, 3};
  int* p = &i;
  const int* cp = &ci;
  S s{1};
  if (f(i) != 1) return 1;
  if (f(&i) != 2) return 2;
  if (f(p) != 2) return 3;
  if (f(&ci) != 3) return 4;
  if (f(cp) != 3) return 5;
  if (f(arr) != 2) return 6;
  if (f(&s) != 2) return 7;
  if (f(&p) != 2) return 8;
  if (g(p) != 1) return 9;
  if (g(&p) != 2) return 10;
  if (g(&cp) != 2) return 11;
  return 0;
}

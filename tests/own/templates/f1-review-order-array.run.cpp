// ζ f1 (#88), from the step's review: partial ordering with a non-type parameter deduced from
// an array bound: f(int(&)[N]) is more specialized than f(T(&)[N]) ([temp.func.order],
// [temp.deduct.type]/8).
// EXIT: 0
template<class T, int N> int f(T (&)[N]) { return 1; }
template<int N> int f(int (&)[N]) { return 2; }
int main() {
  int a[3];
  char c[2];
  if (f(a) != 2) return 1;
  if (f(c) != 1) return 2;
  return 0;
}

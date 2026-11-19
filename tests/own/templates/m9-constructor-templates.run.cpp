// ζ m9 (#88, #314): constructor templates ([temp.mem]/1, [class.ctor]): chosen by deduction as
// other constructors are; never a copy constructor ([class.copy.ctor]/5), so a copy uses the
// implicit one. The iterator-pair form every container has.
// EXIT: 0
struct S {
  int v;
  template<class T> S(T t) : v(int(t) * 2) {}
  S() : v(1) {}
};
template<class U> struct V {
  U n;
  template<class It> V(It first, It last) : n(U(last - first)) {}
};
int main() {
  S a(3);
  S b;
  S c(a);
  int arr[5];
  V<long> v(arr, arr + 5);
  return a.v == 6 && b.v == 1 && c.v == 6 && v.n == 5 ? 0 : 1;
}

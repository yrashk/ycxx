// #340 item 3: a conversion function template makes a function viable through a user-defined
// conversion sequence ([over.best.ics]/2, [over.ics.user]), and an exact match beats it
// ([over.ics.rank]/2).
// EXIT: 0
template <class T> struct R {
  T t;
  template <class U> constexpr operator R<U>() const { return {U(t) + 1}; }
};
long take(R<long> r) { return r.t; }
int pick(R<int>) { return 1; }
int pick(R<short>) { return 2; }
int scalar(double d) { return int(d); }
struct S {
  template <class T> operator T() const { return T(9); }
};
int main() {
  R<int> a{4};
  S s;
  return take(a) == 5 && pick(a) == 1 && scalar(s) == 9 ? 0 : 1;
}

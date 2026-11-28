// #340 item 3: a conversion function template converts to a non-class type ([over.match.conv]);
// the target's top-level cv-qualifiers are ignored, and a pointer target can be reached by a
// qualification conversion ([temp.deduct.conv]/3, /5.3).
// EXIT: 0
struct S {
  int n;
  template <class T> operator T() const { return T(n); }
};
struct P {
  int* p;
  template <class T> operator T*() const { return static_cast<T*>(p); }
};
template <class C> struct W {
  C c;
  template <class T> constexpr operator T() const { return T(c) * 2; }
};
int main() {
  S s{7};
  int i = s;
  const long l = s;
  double x = s;
  int k = 3;
  P p{&k};
  int* q = p;
  const int* r = p;
  W<short> w{5};
  long z = w;
  return i == 7 && l == 7 && x == 7.0 && q == &k && r == &k && z == 10 ? 0 : 1;
}

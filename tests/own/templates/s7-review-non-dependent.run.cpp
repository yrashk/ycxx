// ζ s7 (#88), from the step's review: a static const member whose initializer is not value-dependent is a constant in the
// template itself, usable as an array bound ([temp.dep.constexpr]/2.3).
// EXIT: 0
template<class T> struct S {
  static const int k = 3;
  int a[k];
};
int main() { S<int> s; return sizeof(s.a) == 12 ? 0 : 1; }

// #340 item 3: overload resolution among conversion functions ([over.match.best.general]/2): the
// second standard conversion sequence decides first (/2.2), then a non-template beats a template
// specialization (/2.4), then the more specialized template by return type (/2.5,
// [temp.deduct.partial]/3.2).
// EXIT: 0
struct A {
  template <class T> operator T() const { return T(1); }
  template <class T> operator T*() const { return nullptr; }
  operator long() const { return 2; }
};
struct B {
  operator int() const { return 3; }
  template <class T> operator T() const { return T(4); }
};
struct C {
  operator short() const { return 5; }
  operator double() const { return 6.5; }
};
int main() {
  A a;
  int* p = a;    // T* is more specialized than T
  long l = a;    // the non-template
  char c = a;    // the template's exact match beats long -> char
  B b;
  int i = b;     // the non-template
  C k;
  int s = k;     // short -> int is a promotion, double -> int a conversion
  return p == nullptr && l == 2 && c == 1 && i == 3 && s == 5 ? 0 : 1;
}

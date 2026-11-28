// #340 item 3: a constrained conversion function template is a candidate only when its
// constraints are satisfied after deduction ([temp.deduct.general]/5, [over.match.viable]/3),
// including a type-constraint and a trailing requires-clause; of two otherwise equal ones the
// more constrained wins ([temp.func.order]/6, [over.match.best.general]/2.6).
// EXIT: 0
template <class T> concept small = sizeof(T) <= 2;
template <class T> concept tiny = small<T> && sizeof(T) == 1;
template <class T> struct R {
  T t;
};
struct S {
  template <small T> operator T() const { return T(1); }
  template <class T> requires (sizeof(T) > 2) operator T() const { return T(2); }
};
template <class C> struct M {
  template <class U> requires small<U> constexpr operator R<U>() const { return {U(3)}; }
  template <class U> requires tiny<U> constexpr operator R<U>() const { return {U(4)}; }
  template <class U> requires (sizeof(U) > sizeof(C)) constexpr operator R<U>() const { return {U(5)}; }
};
int main() {
  S s;
  short a = s;
  long b = s;
  M<int> m;
  R<short> c = m;
  R<char> d = m;
  R<long long> e = m;
  return a == 1 && b == 2 && c.t == 3 && d.t == 4 && e.t == 5 ? 0 : 1;
}

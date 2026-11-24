// #340 item 2, from the review: a class template's member defined outside the class names a
// member alias template through the current instantiation, with and without `template`
// ([temp.dep.type]/1, [temp.names]/3), and the definition matches the member's declaration. A
// constructor template, a templated member named as the class, does not hide the class's name.
// EXIT: 0
template <class T> struct Y {
  template <class U> using apply = U*;
  apply<int> f();
  apply<int> g(apply<T>);
};
template <class T> typename Y<T>::template apply<int> Y<T>::f() { return nullptr; }
template <class T> Y<T>::apply<int> Y<T>::g(apply<T> p) { return p ? nullptr : nullptr; }
struct R {
  int value;
  template <class Tag = void> R(int v) : value(v) {}
  R& self() { return *this; }
};
int main() {
  if (R(3).self().value != 3) return 2;
  Y<int> y;
  int i = 0;
  return y.f() == nullptr && y.g(&i) == nullptr ? 0 : 1;
}

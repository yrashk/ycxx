// #340 item 2: member alias templates ([temp.alias], [temp.mem]) of a class and of a class
// template, named as `C::M<args>` and `C::template M<args>` ([temp.names]/4), and by their own
// name in later members of the class.
// EXIT: 0
struct X {
  template <class U> using apply = U;
  template <class U> using ptr = U*;
  apply<long> member = 3;
  ptr<int> p = nullptr;
};
template <class A> struct Y {
  template <class U> using first = A;
  template <class U> using array = U[2];
  template <class U> using fn = A (*)(U);
  array<char> bytes;
};
int main() {
  X::apply<int> r = 1;
  X::template apply<short> s = 2;
  int i = 4;
  X::ptr<int> q = &i;
  Y<char>::array<int> arr = {5};
  Y<long>::first<int> l = 6;
  static_assert(sizeof(Y<double>::array<char>) == 2);
  Y<int> y;
  Y<int>::fn<long> g = nullptr;
  X x;
  return (g ? 1 : 0) + r + s + int(x.member) + *q + arr[0] + int(l) + int(sizeof(y.bytes)) == 23 ? 0 : 1;
}

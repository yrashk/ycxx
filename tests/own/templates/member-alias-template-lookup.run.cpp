// #340 item 2: more uses of member alias templates ([temp.mem], [temp.alias]): before `::` in a
// nested-name-specifier, inherited from a base ([class.member.lookup]) by name and qualified, in
// a member function's body, with packs ([temp.variadic]).
// EXIT: 0
template <class A> struct Box {
  using type = A;
};
struct X {
  template <class U> using wrap = Box<U>;
};
template <class T> struct Y {
  template <class U> using wrap = Box<U>;
  template <class U> using same = U;
  static int f() {
    same<int> x = 3;
    typename Y::template same<long> y = 4;
    return x + int(y);
  }
};
template <class T> using R = typename Y<T>::template wrap<T>::type;
struct B {
  template <class U> using ptr = U*;
};
struct D : B {
  ptr<int> p = nullptr;
};
template <class T> struct E : B {
  ptr<T> q = nullptr;
};
template <class... Ts> struct List {};
struct P {
  template <class... Us> using list = List<Us...>;
};
template <class Q, class... Ts> using L = typename Q::template list<Ts..., int>;
template <class... Ts> constexpr int count(List<Ts...>) { return sizeof...(Ts); }
int main() {
  X::wrap<int>::type a = 1;
  R<long> b = 2;
  D::ptr<long> x = nullptr;
  D d;
  E<int> e;
  static_assert(count(L<P, char, long>{}) == 3 && count(P::list<>{}) == 0);
  if (x || d.p || e.q) return 1;
  return int(a + b) + Y<char>::f() == 10 ? 0 : 2;
}

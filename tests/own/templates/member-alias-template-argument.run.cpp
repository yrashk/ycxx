// #340 item 2: a member alias template as a template template argument ([temp.arg.template]),
// named as `C::M`, `C::template M` and, through a dependent qualifier, `Q::template M`
// ([temp.names]/4), as libycxx's common_reference passes `__xref<T>::template apply`.
// EXIT: 0
template <template <class> class F> struct Use {
  using type = F<int>;
};
struct X {
  template <class U> using apply = U*;
};
template <class A> struct Y {
  template <class U> using apply = A;
};
template <class T> using R = typename Use<Y<T>::template apply>::type;
template <class T> struct Z {
  using type = typename Use<T::template apply>::type;
};
template <class A, class B, template <class> class FA, template <class> class FB> struct Both {
  using type = FA<B>;
};
template <class T1, class T2> using Common = typename Both<T1, T2, Y<T1>::template apply, Y<T2>::template apply>::type;
int main() {
  int i = 3;
  Use<X::apply>::type p = &i;
  Use<X::template apply>::type p2 = &i;
  R<long> l = 4;
  Use<Y<char>::apply>::type c = 5;
  typename Z<X>::type p3 = &i;
  Common<short, int> s = 6;
  static_assert(sizeof(Common<char, long>) == 1);
  return *p + *p2 + int(l) + c + *p3 + s == 24 ? 0 : 1;
}

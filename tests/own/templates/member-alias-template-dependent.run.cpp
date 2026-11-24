// #340 item 2: a member alias template named through a dependent qualifier,
// `typename Q::template M<args>` ([temp.names]/4, [temp.res.general]/5): in an alias template,
// a class template's member and a function template's return type, substituted once the
// qualifier is known ([temp.alias]/2).
// EXIT: 0
template <class A> struct Y {
  template <class U> using apply = U*;
  template <class U> using first = A;
};
struct X {
  template <class U> using apply = U;
};
template <class Q, class T> struct Use {
  using type = typename Q::template apply<T>;
};
template <class A, class U> using R = typename Y<A>::template apply<U>;
template <class T> typename Y<T>::template first<int> f(T t) { return t + 1; }
template <class T> struct Holder {
  typename Y<T>::template apply<T> p;
};
int main() {
  int i = 7;
  R<char, int> p = &i;
  typename Use<X, long>::type l = 3;
  typename Use<Y<int>, int>::type q = &i;
  Holder<int> h{&i};
  return *p + int(l) + *q + *h.p + int(f(1L)) == 26 ? 0 : 1;
}

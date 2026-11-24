// The peer of member-alias-template-mangling.run.cpp: it declares the test's function
// templates and calls the specializations the test instantiates.
template <class A> struct Y {
  template <class U> using first = A;
  template <class U> using apply = U*;
};
template <class T> typename Y<T>::template first<int> f(T t);
template <class T, class U> typename Y<T>::template apply<U> h(T, U* u);
struct X {
  template <class U> using ptr = U*;
  template <class U> using wrap = Y<U>;
  using self = X;
};
template <class T> void two(typename T::template ptr<int>, typename T::template ptr<long>);
template <class T> typename T::template ptr<typename T::template ptr<T>> nest(T);
template <class T> typename T::template wrap<int>::template first<char> inner(T);
template <class T> typename T::self::template ptr<int> self(T);
int peer() {
  int i = 0;
  long l = 0;
  X x;
  two<X>(&i, &l);
  return f(2L) == 3 && h(2L, &i) == &i && nest(x) == nullptr && inner(x) == 5 && self(x) == nullptr ? 0 : 1;
}

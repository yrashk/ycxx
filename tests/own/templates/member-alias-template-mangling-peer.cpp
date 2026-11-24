// The peer of member-alias-template-mangling.run.cpp: it declares the test's function
// templates and calls the specializations the test instantiates.
template <class A> struct Y {
  template <class U> using first = A;
  template <class U> using apply = U*;
};
template <class T> typename Y<T>::template first<int> f(T t);
template <class T, class U> typename Y<T>::template apply<U> h(T, U* u);
int peer() {
  int i = 0;
  return f(2L) == 3 && h(2L, &i) == &i ? 0 : 1;
}

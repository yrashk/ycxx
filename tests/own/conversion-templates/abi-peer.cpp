// The peer of abi.run.cpp, compiled by the oracle: it defines the conversion function template
// specializations the test calls.
struct S {
  int n;
  template <class T> operator T() const;
};
template <class T> S::operator T() const { return T(n) + 1; }
template S::operator long() const;
template <class T> struct R {
  T t;
  template <class U> operator R<U>() const&;
};
template <class T> template <class U> R<T>::operator R<U>() const& { return {U(t + 2)}; }
template R<int>::operator R<long>() const&;

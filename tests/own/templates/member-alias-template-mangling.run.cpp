// #340 item 2: function templates whose signatures name a member alias template through a
// dependent qualifier, as `typename Q::template M<args>` and as the template template argument
// `Q::template M` ([temp.names]/4, [temp.over.link]). They are mangled as the Itanium ABI says
// (§5.1.5.3, §5.1.10): the peer, compiled by the oracle, calls the specializations this test
// instantiates. `g`'s argument `Y<T>::template apply` is mangled as GCC does, `N1YIT_E5applyE`;
// Clang writes `sr1YIT_EE5applyE`, so the peer does not call it (DECISIONS §1.7).
// PEER: member-alias-template-mangling-peer.cpp
// EXIT: 0
template <class A> struct Y {
  template <class U> using first = A;
  template <class U> using apply = U*;
};
template <template <class> class F> struct Use {
  using type = F<int>;
};
template <class T> typename Y<T>::template first<int> f(T t) { return t + 1; }
template <class T> typename Use<Y<T>::template apply>::type g(T) { return nullptr; }
template <class T, class U> typename Y<T>::template apply<U> h(T, U* u) { return u; }
int peer();
int main() {
  int i = 0;
  if (f(1L) != 2 || g(1L) != nullptr || h(1L, &i) != &i) return 1;
  return peer();
}

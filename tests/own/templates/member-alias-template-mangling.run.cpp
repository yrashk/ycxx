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
// A repeated template prefix is a substitution, and a qualifier that is itself a dependent name
// is its prefix, not a nested `N…E` (§5.1.5.3).
struct X {
  template <class U> using ptr = U*;
  template <class U> using wrap = Y<U>;
  using self = X;
};
template <class T> void two(typename T::template ptr<int>, typename T::template ptr<long>) {}
template <class T> typename T::template ptr<typename T::template ptr<T>> nest(T) { return nullptr; }
template <class T> typename T::template wrap<int>::template first<char> inner(T) { return 5; }
template <class T> typename T::self::template ptr<int> self(T) { return nullptr; }
int peer();
int main() {
  int i = 0;
  long l = 0;
  X x;
  two<X>(&i, &l);
  if (f(1L) != 2 || g(1L) != nullptr || h(1L, &i) != &i) return 1;
  if (nest(x) != nullptr || inner(x) != 5 || self(x) != nullptr) return 2;
  return peer();
}

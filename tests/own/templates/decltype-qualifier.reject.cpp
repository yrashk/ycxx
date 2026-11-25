// A decltype-specifier before `::` denotes a class, an enumeration or a dependent type
// ([expr.prim.id.qual]/1), and its members are looked up once it is known ([temp.res.general]). (#1243)
template <class T> struct S { using type = T; static constexpr int value = 1; };
template <class T> S<T> p();
template <class T> S<T>& reference();
template <class T> auto deduced() { return T(); }

using A = decltype(reference<int>())::type; //! error
using B = decltype(p<int>())::missing; //! error
using C = decltype(undeclared)::type; //! error
using D = typename decltype(p<int>())::value; //! error
int e = decltype(p<int>())::missing; //! error

template <class T> struct F { using type = typename decltype(reference<T>())::type; }; F<int>::type f; //! error
template <class T> struct G { using type = typename decltype(deduced<T>())::type; }; G<int>::type g; //! error
G<S<int>>::type good = 1;
template <class T> using H = typename decltype(deduced<T>())::type; H<int> h; //! error
H<S<int>> good_alias = 1;
template <class T> int local() {
  decltype(p<T>())::type x; //! error
  return 0;
}
struct Own { static auto pick() { return S<int>{}; } using type = decltype(pick()); }; //! error

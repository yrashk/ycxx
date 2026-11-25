// A decltype-specifier as a base must denote a class ([class.derived.general]/2); a dependent one
// is checked when the template is instantiated ([temp.inst]). (#1243)
template <class T> struct S {};
template <class T> S<T>& reference();
template <class T> T value();
template <class T> auto deduced() { return T(); }

struct A : decltype(reference<int>()) {}; //! error
struct B : decltype(1) {}; //! error

template <class T> struct C : decltype(value<T>()) {}; C<int> c; //! error
C<S<int>> good;
template <class T> struct D : decltype(deduced<T>()) {}; D<int> d; //! error
D<S<int>> good_deduced;
template <class T> struct E : decltype(reference<T>()) {}; E<int> e; //! error
struct Final final {};
template <class T> struct F : decltype(deduced<T>()) {}; F<Final> f; //! error
struct Incomplete;
template <class T> struct G : decltype(value<T>()) {}; G<Incomplete> g; //! error
template <class T> struct H : decltype(T::missing) {}; H<S<int>> h; //! error
template <class T> struct I : const decltype(value<T>()) {}; //! error

// [basic.lookup.general], [basic.lookup.qual], [namespace.def]
template<int N> struct Constant {};
template<int N> using Base = Constant<N>;

namespace Scope {
template<class T> consteval int probe() { return sizeof(T); }
inline namespace Inner { struct probe {}; }
}

template<class T> struct Derived : Base<Scope::probe<T>()> {}; //! error
Derived<char> instance; //! error

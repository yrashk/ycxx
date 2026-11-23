// [basic.lookup.general], [basic.lookup.qual], [namespace.def], [temp.arg.nontype]
template<unsigned N> struct Constant {};
template<unsigned N> using Base = Constant<N>;

namespace Numbers {
template<class T> consteval unsigned measure() { return sizeof(T); }
inline namespace Detail { struct measure {}; }
}

struct Derived : Base<Numbers::measure<char>()> {}; //! error

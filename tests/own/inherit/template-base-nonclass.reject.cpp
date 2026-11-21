// The completeness demand checks that the substituted base denotes a class ([temp.inst], #308).
template<class Base> struct Derived : Base {}; static_assert(sizeof(Derived<int>) > 0); //! error

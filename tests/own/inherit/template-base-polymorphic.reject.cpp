// Completion cannot use a polymorphic direct base with the current virtual declaration support (#309).
struct Root { virtual int f(); };
template<class Base> struct Derived : Base {}; static_assert(sizeof(Derived<Root>) > 0); //! error

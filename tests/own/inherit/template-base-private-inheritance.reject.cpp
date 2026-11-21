// Access to the original member and to the inherited name are both checked ([class.access.base]).
struct Base { static constexpr int value = 17; };
template<class Parent> struct Derived : private Parent {};

[[maybe_unused]] constexpr int result = Derived<Base>::value; //! error

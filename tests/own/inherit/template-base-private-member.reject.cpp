// The inherited name preserves the declaring base's member access ([class.access.general]).
class Base { static constexpr int value = 17; };
template<class Parent> struct Derived : Parent {};

[[maybe_unused]] constexpr int result = Derived<Base>::value; //! error

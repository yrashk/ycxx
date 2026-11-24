// #218: a dependent query is decided where it is instantiated; Derived is still incomplete there.
template <class Base, class Derived>
inline constexpr bool is_virtual_base_of_v = __builtin_is_virtual_base_of(Base, Derived); //! error
struct A {};
struct Late;
bool demand = is_virtual_base_of_v<A, Late>;
struct Late : A {};

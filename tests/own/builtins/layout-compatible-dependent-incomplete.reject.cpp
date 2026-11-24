// #218: a dependent query is decided where it is instantiated; an operand is still incomplete there.
template <class T, class U>
inline constexpr bool is_layout_compatible_v = __is_layout_compatible(T, U); //! error
struct Late;
bool demand = is_layout_compatible_v<int, Late>;
struct Late { int x; };

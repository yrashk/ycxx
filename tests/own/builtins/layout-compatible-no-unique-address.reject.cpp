// XFAIL: clang,gcc Clang 23 answers false and GCC 16 true; ycxx does not decide yet (#1141).
// #218: [class.mem.general] ends a struct's common initial sequence at a `[[no_unique_address]]`
// member only where `__has_cpp_attribute(no_unique_address)` is nonzero.
struct E {};
struct NU1 { [[no_unique_address]] E e; int a; };
struct NU2 { [[no_unique_address]] E e; int a; };
constexpr bool v = __is_layout_compatible(NU1, NU2); //! error

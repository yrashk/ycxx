// #218: dependent `__builtin_is_virtual_base_of` operands in a class-template argument and a
// variable-template initializer, as libycxx's <type_traits> has them, are decided at
// instantiation, with Derived's completeness judged there ([temp.point]).
template <bool B> struct Answer { static constexpr bool value = B; };
template <class Base, class Derived>
struct is_virtual_base_of : Answer<__builtin_is_virtual_base_of(Base, Derived)> {};
template <class Base, class Derived>
inline constexpr bool is_virtual_base_of_v = __builtin_is_virtual_base_of(Base, Derived);

struct A {};
struct Late;
static_assert(!is_virtual_base_of<int, A>::value && !is_virtual_base_of_v<A, A>);
struct Late : A {};
static_assert(!is_virtual_base_of<A, Late>::value && !is_virtual_base_of_v<A, Late>);
static_assert(!is_virtual_base_of<const A, volatile Late>::value);

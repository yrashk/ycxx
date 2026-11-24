// #218: dependent `__is_pointer_interconvertible_base_of` operands in a class-template argument
// and a variable-template initializer, as libycxx's <type_traits> has them, are decided at
// instantiation, with Derived's completeness judged there ([temp.point]).
template <bool B> struct Answer { static constexpr bool value = B; };
template <class Base, class Derived>
struct is_pointer_interconvertible_base_of : Answer<__is_pointer_interconvertible_base_of(Base, Derived)> {};
template <class Base, class Derived>
inline constexpr bool is_pointer_interconvertible_base_of_v = __is_pointer_interconvertible_base_of(Base, Derived);

struct A {};
struct F { int x; };
struct Late;
static_assert(is_pointer_interconvertible_base_of<Late, Late>::value && !is_pointer_interconvertible_base_of_v<int, A>);
struct Late : A { int y; };
struct Mixed : F { int z; };
static_assert(is_pointer_interconvertible_base_of<A, Late>::value && is_pointer_interconvertible_base_of_v<const A, Late>);
static_assert(!is_pointer_interconvertible_base_of<F, Mixed>::value && !is_pointer_interconvertible_base_of_v<Late, A>);

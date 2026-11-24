// #218: dependent `__builtin_is_virtual_base_of`, `__is_layout_compatible` and
// `__is_pointer_interconvertible_base_of` in a concept and a requires-clause are evaluated when
// the constraint's satisfaction is checked ([temp.constr.atomic]).
template <class B, class D> concept PIB = __is_pointer_interconvertible_base_of(B, D);
template <class B, class D> concept VB = __builtin_is_virtual_base_of(B, D);
template <class T, class U> requires __is_layout_compatible(T, U) constexpr int f() { return 1; }
template <class T, class U> constexpr int f() { return 0; }
struct A {};
struct D : A { int x; };
struct E { int y; };
static_assert(PIB<A, D> && !PIB<D, A> && !VB<A, D> && !VB<int, int>);
static_assert(f<D, E>() == 1 && f<D, A>() == 0 && f<int, const int>() == 1);

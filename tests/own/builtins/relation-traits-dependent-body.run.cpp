// #218: `__builtin_is_virtual_base_of`, `__is_layout_compatible` and
// `__is_pointer_interconvertible_base_of` with dependent operands in function template bodies,
// decided when the template is instantiated ([temp.inst]).
// EXIT: 0
struct A {};
struct D : A { int x; };
struct E { int y; };
template <class B, class T> constexpr bool virtual_base() { return __builtin_is_virtual_base_of(B, T); }
template <class T, class U> constexpr bool layout() { return __is_layout_compatible(T, U); }
template <class B, class T> constexpr bool interconvertible() { return __is_pointer_interconvertible_base_of(B, T); }
static_assert(!virtual_base<A, D>() && layout<D, E>() && interconvertible<A, D>());
int main() {
  if (virtual_base<A, D>() || !layout<D, const E>() || layout<D, A>() || !interconvertible<A, D>() || interconvertible<D, A>())
    return 1;
  return 0;
}

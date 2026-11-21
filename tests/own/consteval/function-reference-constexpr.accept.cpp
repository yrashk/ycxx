// #102, [expr.const.const], [expr.const.imm]: constexpr variables can hold immediate pointers.
// XFAIL: clang,gcc Clang 23 and GCC 16 reject pointers to immediate functions in constexpr initializers.
consteval int immediate(int x) { return x + 1; }

constexpr auto namespace_pointer = immediate;
static_assert(namespace_pointer(41) == 42);

void local() {
  constexpr auto local_pointer = &immediate;
  static_assert(local_pointer(41) == 42);
}

using Pointer = int(*)(int);
template<class P> consteval P stored_default(P p = immediate) { return p; }
constexpr Pointer default_pointer = stored_default<Pointer>();
static_assert(default_pointer(41) == 42);

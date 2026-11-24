// XFAIL: clang,gcc Clang 23 and GCC 16 require a complete class even when Base and Derived are the same class.
// #218: [meta.rel] requires a complete Derived only when Base and Derived are distinct nonunion
// class types; a class is not its own virtual base.
struct Inc;
static_assert(!__builtin_is_virtual_base_of(Inc, Inc));
static_assert(!__builtin_is_virtual_base_of(const Inc, Inc));

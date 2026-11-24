// #218: [meta.rel] requires a complete Derived when Base and Derived are distinct class types.
struct A {};
struct Inc;
static_assert(!__builtin_is_virtual_base_of(A, Inc)); //! error

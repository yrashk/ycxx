// #218: [meta.rel] requires a complete Derived when both operands are nonunion class types.
struct Inc;
static_assert(!__builtin_is_virtual_base_of(const Inc, Inc)); //! error

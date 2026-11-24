// #218: [meta.rel] requires complete types, cv void or arrays of unknown bound.
struct Inc;
static_assert(__is_layout_compatible(Inc, Inc)); //! error

// [expr.alignof]
struct Pending;
static_assert(alignof(Pending) > 0); //! error
struct Pending {};

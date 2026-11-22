// #218: A user-provided eligible copy constructor makes the class nontrivially copyable.
struct Value { Value(const Value&) {} };
static_assert(!__is_trivially_copyable(Value));

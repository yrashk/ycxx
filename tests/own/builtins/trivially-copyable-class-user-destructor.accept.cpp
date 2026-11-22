// #218: A user-provided destructor prevents trivial copyability.
struct Value { ~Value() {} };
static_assert(!__is_trivially_copyable(Value));

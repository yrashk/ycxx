// #218: A user-provided ordinary default constructor leaves eligible trivial copy operations.
struct Value { Value() {} int payload; };
static_assert(__is_trivially_copyable(Value));

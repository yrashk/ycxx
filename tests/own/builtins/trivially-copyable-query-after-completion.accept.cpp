// #218: A non-dependent query after the definition sees a complete ordinary class.
struct Value;
struct Value { int payload; };
static_assert(__is_trivially_copyable(Value));

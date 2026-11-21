// #218: An array of a complete proved class can have unknown bound.
struct Value { int bits; };
static_assert(__is_trivially_copyable(Value[]));

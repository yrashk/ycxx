// #218: A scoped enumeration is a scalar type.
enum class E : unsigned char { start };
static_assert(__is_trivially_copyable(E));

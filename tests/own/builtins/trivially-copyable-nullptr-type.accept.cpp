// #218: The type of nullptr is a scalar type.
static_assert(__is_trivially_copyable(decltype(nullptr)));

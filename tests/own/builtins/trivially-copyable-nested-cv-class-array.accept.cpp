// #218: A bounded multidimensional array uses its complete cv-qualified class element.
struct Value { int bits; };
static_assert(__is_trivially_copyable(const volatile Value[2][3]));

// #218: The unknown outer bound does not change the ultimate scalar element property.
static_assert(__is_trivially_copyable(const volatile unsigned char[][3]));

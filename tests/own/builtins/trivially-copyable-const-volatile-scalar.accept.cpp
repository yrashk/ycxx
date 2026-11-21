// #218: Arithmetic types are trivially copyable even when const-volatile.
static_assert(__is_trivially_copyable(const volatile unsigned long));

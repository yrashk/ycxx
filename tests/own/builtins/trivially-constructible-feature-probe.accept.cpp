// XFAIL: clang,gcc Peers advertise the full trait
#if __has_builtin(__is_trivially_constructible) != 0
#error The partial trait must not advertise full builtin support
#endif
static_assert(__is_trivially_constructible(int));

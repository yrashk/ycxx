// XFAIL: clang,gcc These peers advertise their constructibility builtin.
#if __has_builtin(__is_constructible)
#error constructibility advertised before the trait is fully supported
#endif
int builtin_probe;

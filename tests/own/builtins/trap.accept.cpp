#if !__has_builtin(__builtin_trap)
#error __builtin_trap is unavailable
#endif

static_assert(__is_same(decltype(__builtin_trap()), void));

void stop() { __builtin_trap(); }

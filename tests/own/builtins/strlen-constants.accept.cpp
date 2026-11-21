#if !__has_builtin(__builtin_strlen)
#error __builtin_strlen is unavailable
#endif

decltype(sizeof(0)) length_result_type();
decltype(__builtin_strlen("abc")) length_result_type();

static_assert(__builtin_strlen("") == 0);
static_assert(__builtin_strlen("abc") == 3);
static_assert(__builtin_strlen("ab\0cd") == 2);

constexpr char text[] = {'x', 'a', 'b', '\0', 'z', '\0'};
static_assert(__builtin_strlen(text) == 3);
static_assert(__builtin_strlen(text + 1) == 2);
static_assert(__builtin_strlen(text + 3) == 0);
static_assert(__builtin_strlen(text + 4) == 1);

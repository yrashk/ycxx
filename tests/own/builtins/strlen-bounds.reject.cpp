constexpr char text[] = {'x', 'a', 'b'};
static_assert(__builtin_strlen(text + 1) == 2); //! error

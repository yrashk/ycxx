static_assert((false ? (__builtin_trap(), 91) : 37) == 37);
static_assert((true ? (__builtin_trap(), 91) : 37) == 91); //! error

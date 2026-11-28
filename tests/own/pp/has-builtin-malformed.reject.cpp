// Malformed `__has_builtin` operands in ordinary expressions preserve byte offsets (#1626).
/* αβγ */ int empty = __has_builtin(); //! error[clang]@2:40 //! error[ycxx]@2:40 //! error[gcc]@2:37
int non_id = \
__has_builtin(123); //! error@4:15
int extra_comma = __has_builtin(__builtin_memcmp, 1); //! error@5:49
int scoped = __has_builtin(ns::name); //! error@6:30
int missing_open = __has_builtin 0; //! error@7:34

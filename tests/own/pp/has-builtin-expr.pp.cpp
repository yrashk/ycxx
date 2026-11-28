// `-E` replaces `__has_builtin` in ordinary C++ tokens (#1626).
#define PROBE(x) __has_builtin(x)
#define STR(x) #x

int direct_yes = __has_builtin(__builtin_memcmp);
int direct_no = __has_builtin(__ycxx_no_such_builtin);
int macro_yes = PROBE(__builtin_memcmp);
int multiline =
    __has_builtin(
        __builtin_memcmp
    )
;
int after = 5;
const char* spelled = STR(__has_builtin(__builtin_memcmp));

#undef __has_builtin
#ifdef __has_builtin
int undef_ifdef = 1;
#else
int undef_ifdef = 0;
#endif
int __has_builtin = 19;

#define __has_builtin(x) 42
int custom_expr = __has_builtin(__ycxx_no_such_builtin);
#if __has_builtin(__ycxx_no_such_builtin) == 42
int custom_if = 42;
#endif


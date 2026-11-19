/* Compiler types and macros ([support.types.layout], [support.types.nullptr]).
   glibc can ask for just size_t, wchar_t or NULL, in any order. A partial inclusion
   leaves the full inclusion available to libycxx's <cstddef>. */
#if !defined(__need_size_t) && !defined(__need_wchar_t) && !defined(__need_NULL)
#  ifndef __YCXX_STDDEF_H
#    define __YCXX_STDDEF_H
#    define __need_size_t
#    define __need_wchar_t
#    define __need_NULL

typedef __PTRDIFF_TYPE__ ptrdiff_t;

/* The AMD64 psABI §3.1.2 requires 16-byte alignment. Keep the Linux toolchain's
   32-byte size by placing an integer before the 16-byte long double. */
typedef struct {
  long long __integer;
  long double __floating;
} max_align_t;

#    define offsetof(__type, __member) __builtin_offsetof(__type, __member)
#  endif
#endif

#ifdef __need_size_t
typedef __SIZE_TYPE__ size_t;
#  undef __need_size_t
#endif

#ifdef __need_wchar_t
#  ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#  endif
#  undef __need_wchar_t
#endif

#ifdef __need_NULL
#  ifndef NULL
#    ifdef __cplusplus
#      define NULL nullptr
#    else
#      define NULL ((void*)0)
#    endif
#  endif
#  undef __need_NULL
#endif

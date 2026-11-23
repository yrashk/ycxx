/* Argument lists ([cstdarg.syn]), for every target: glibc and the macOS SDK both leave this
   header to the compiler. glibc can request its internal alias on either side of a full
   inclusion; each request, including a repeat, is consumed. */
#ifdef __need___va_list
#  undef __need___va_list
#else
#  ifndef _VA_LIST_DEFINED
#    define _VA_LIST_DEFINED
typedef __builtin_va_list va_list;
#  endif

/* The same replacement lists as libycxx's <cstdarg>, so the headers combine in either order
   ([cpp.replace.general]). va_start(V, ...) ignores everything after V. */
#  define va_start(_Vp, ...) __builtin_c23_va_start(_Vp)
#  define va_arg(_Vp, _Pp) __builtin_va_arg(_Vp, _Pp)
#  define va_copy(_VDST, _VSRC) __builtin_va_copy(_VDST, _VSRC)
#  define va_end(_Vp) __builtin_va_end(_Vp)
#  define __va_copy(_VDST, _VSRC) __builtin_va_copy(_VDST, _VSRC)
#  ifndef __STDC_VERSION_STDARG_H__
#    define __STDC_VERSION_STDARG_H__ 202311L
#  endif
#endif

#ifndef __GNUC_VA_LIST
#  define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
#endif

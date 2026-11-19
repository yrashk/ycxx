/* Argument list type ([cstdarg.syn]). glibc can request its internal alias on
   either side of a full inclusion. Consume each request, including repeats. */
#ifdef __need___va_list
#  undef __need___va_list
#elif !defined(_VA_LIST_DEFINED)
#  define _VA_LIST_DEFINED
typedef __builtin_va_list va_list;
#endif

#ifndef __GNUC_VA_LIST
#  define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
#endif

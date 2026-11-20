#pragma once

#if __has_include_next(<limits.h>) && __has_include(<features.h>)
#  include <features.h>
#endif

#if defined(__GLIBC__) || !__has_include_next(<limits.h>)
/* Share the macro-only target limits used by <climits> ([climits.syn]). */
#  ifndef _YCXX_LLONG_WIDTH
#    define _YCXX_LLONG_WIDTH __LLONG_WIDTH__
#  endif
#  include <ycxx/core/climits.hpp>
#endif

#if __has_include_next(<limits.h>)
#  ifdef __GLIBC__
/* glibc supplies locale and POSIX limits. It expects the compiler's ISO
   limits first and uses this marker to end the compiler-header search. */
#    ifndef _LIBC_LIMITS_H_
#      undef MB_LEN_MAX
#    endif
#    ifdef _GCC_LIMITS_H_
#      include_next <limits.h>
#    else
#      define _GCC_LIMITS_H_ 1
#      include_next <limits.h>
#      undef _GCC_LIMITS_H_
#    endif
#  else
#    include_next <limits.h>
#  endif
#endif

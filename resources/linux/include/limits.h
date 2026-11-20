#pragma once

#if __has_include_next(<limits.h>)
#  if __has_include(<features.h>)
#    include <features.h>
#  endif

#  ifdef __GLIBC__
/* glibc supplies locale and POSIX limits. It expects the compiler's ISO
   limits before applying its own fallbacks and uses this marker to avoid
   searching past its header for a second compiler <limits.h>. */
#    include <climits>
#    ifndef _LIBC_LIMITS_H_
/* A fresh glibc <limits.h> supplies its locale bound instead of the fallback. */
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
#else
#  include <climits>
#endif

#pragma once

/* Share libycxx's macro-only limits ([cfloat.syn]), including the rounding
   policy used by <cfloat>. The C header can be included under C linkage. */
#ifndef _YCXX_FLT_ROUNDS
#  if __has_builtin(__builtin_flt_rounds)
#    define _YCXX_FLT_ROUNDS (__builtin_flt_rounds())
#  else
#    define _YCXX_FLT_ROUNDS 1
#  endif
#endif
#include <ycxx/core/cfloat.hpp>

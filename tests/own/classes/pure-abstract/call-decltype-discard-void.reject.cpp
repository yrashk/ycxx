// XFAIL: clang Clang 23 accepts the abstract prvalue cast to void in decltype.
#include "abstract.hpp"

Abstract make();
// [expr.context] materializes the abstract prvalue discarded by the cast to void.
using Result = decltype(static_cast<void>(make()));  //! error

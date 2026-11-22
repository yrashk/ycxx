// XFAIL: clang Clang 23 accepts the abstract receiver inside decltype.
#include "abstract.hpp"

Abstract make();
// [expr.ref] materializes the prvalue receiver of the nonstatic member call.
using Result = decltype(make().score(1));  //! error

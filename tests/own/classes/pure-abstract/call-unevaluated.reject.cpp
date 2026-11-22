#include "abstract.hpp"

Abstract& source();
void take(Abstract);

// [dcl.type.decltype]/2 does not remove the need to initialize the argument.
using Result = decltype(take(source()));  //! error

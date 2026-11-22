// XFAIL: clang Clang 23 rejects the declaration of an abstract object that is not defined.
#include "abstract.hpp"

// [basic.def] permits a declaration with extern and no initializer.
extern Abstract object;

// XFAIL: clang Clang 23 accepts an evaluated call with an abstract return type.
#include "abstract.hpp"

Abstract make();
void run() {
    make();  //! error
}

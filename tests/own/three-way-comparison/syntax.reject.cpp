// [expr.spaceship]: `<=>` is a binary operator, with a shift-expression on each side.
#include "inputs/compare.hpp"

void f(int a) {
    (void)(a <=> );  //! error
    (void)(<=> a);   //! error
    (void)(a <=> <=> a);  //! error
}

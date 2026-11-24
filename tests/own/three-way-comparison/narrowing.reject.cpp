// [expr.spaceship]/4: a narrowing conversion other than from an integral type to a floating-point
// type makes the comparison ill-formed: a signed operand converted to unsigned.
#include "inputs/compare.hpp"

bool f(int i, unsigned u, long l, unsigned long ul, unsigned short us) {
    (void)(us <=> 1u);  // promoted to int, then a constant that fits
    (void)(i <=> 1.5);  // integral to floating: allowed
    (void)(-1 <=> 1u);  //! error
    (void)(l <=> ul);   //! error
    return (i <=> u) < 0;  //! error
}

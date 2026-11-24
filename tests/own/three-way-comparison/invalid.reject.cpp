// [expr.spaceship]: operands of no matching kinds make the program ill-formed.
#include "inputs/compare.hpp"

enum A { a };
enum B { b };
enum class S { s };
void f();
void g();

void test(int* p, int i, double d, bool t, int (&x)[2], int (&y)[2]) {
    (void)(a <=> b);        //! error
    (void)(S::s <=> 0);     //! error
    (void)(S::s <=> d);     //! error
    (void)(p <=> nullptr);  //! error
    (void)(p <=> 0);        //! error
    (void)(p <=> i);        //! error
    (void)(p <=> d);        //! error
    (void)(&f <=> &g);      //! error
    (void)(t <=> i);        //! error
    (void)(a <=> true);     //! error
    (void)(true <=> 1.0);   //! error
    (void)(x <=> y);        //! error
    (void)(x <=> p);        // an array and an object pointer: allowed
}

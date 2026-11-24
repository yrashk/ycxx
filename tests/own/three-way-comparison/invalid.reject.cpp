// [expr.spaceship]: operands of no matching kinds make the program ill-formed.
#include "inputs/compare.hpp"

enum A { a };
enum B { b };
enum class S { s };
void f();
void g();

void test(int* p, int i, double d) {
    (void)(a <=> b);        //! error
    (void)(S::s <=> 0);     //! error
    (void)(S::s <=> d);     //! error
    (void)(p <=> nullptr);  //! error
    (void)(p <=> 0);        //! error
    (void)(p <=> i);        //! error
    (void)(p <=> d);        //! error
    (void)(&f <=> &g);      //! error
}

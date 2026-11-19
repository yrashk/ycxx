// #220, from the review: no integral type holds a value past the largest unsigned one
// ([dcl.enum]/5).
// XFAIL: gcc GCC gives the enumeration a 128-bit underlying type; Clang rejects it, as ycxx does
enum Y { c = 0xFFFFFFFFFFFFFFFFULL, d }; //! error

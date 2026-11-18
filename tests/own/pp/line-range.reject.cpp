// [cpp.line]/3: a line number outside 1..2147483647 is conditionally-supported; neither oracle
// supports it in strict mode (-pedantic-errors), and ycxx follows them. Each is followed by a
// #line restoring the physical numbering.
#line 0 //! error
#line 6
#line 2147483648 //! error
#line 8
int after;

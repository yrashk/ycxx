// [cpp.cond]/1 (the if-section grammar), [cpp.pre]: every #if needs its #endif in the same
// file; one still open at the end of the file is ill-formed, reported at the opening #if.
// XFAIL: gcc GCC reports "unterminated #if" as file:line: with no column, which the driver's (#128)
// diagnostic pattern (file:line:col: error:) does not parse; the verdict and line are right
int before;
#if 1 //! error
int inside;

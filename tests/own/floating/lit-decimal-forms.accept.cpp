// [lex.fcon]: the decimal forms of a floating-point-literal: a fractional-constant with digits on
// either or both sides of the period, an optional exponent-part with e or E and an optional sign,
// and a digit-sequence with a mandatory exponent. Every value here is exact in double.
static_assert(1.0 == 1.);
static_assert(.5 == 0.5);
static_assert(1e2 == 100.0);
static_assert(1E2 == 100.0);
static_assert(1e+2 == 100.0);
static_assert(25e-2 == 0.25);
static_assert(15e-1 == 1.5);
static_assert(.125e3 == 125.);
static_assert(3.e1 == 30.0);
static_assert(0.0 == 0. && 00.0 == 0.0 && 0e0 == .0);
static_assert(007.5 == 7.5); // a leading zero does not make a floating literal octal
static_assert(09.5 == 9.5);
static_assert(1e0 == 1 && 1e1 == 10 && 1e22 == 10000000000000000000000.0);

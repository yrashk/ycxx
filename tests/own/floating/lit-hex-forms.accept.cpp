// [lex.fcon]: a hexadecimal-floating-point-literal's scaled value is s * 2^e, where s is the
// hexadecimal significand and e the decimal binary exponent (the draft's example: 49.625 and
// 0xC.68p+2 have the same value).
static_assert(0xC.68p+2 == 49.625);
static_assert(0x1p0 == 1.0);
static_assert(0X1P0 == 1.0);
static_assert(0x1.8p1 == 3.0);
static_assert(0x.8p1 == 1.0);
static_assert(0x1.p4 == 16.0);
static_assert(0x1p-2 == 0.25);
static_assert(0x1P+10 == 1024.0);
static_assert(0xA.p0 == 10.0 && 0xa.0p0 == 10.0);
static_assert(0x1.fp0 == 0x1.Fp0 && 0x1.fp0 == 1.9375);
static_assert(0x10p-4 == 1.0 && 0x0.1p4 == 1.0);
static_assert(0xffp0 == 255.0);
static_assert(0x1p10 == 0x400p0); // the exponent is decimal: p10 is 2^10
static_assert(0x1.000001p0 > 1.0);

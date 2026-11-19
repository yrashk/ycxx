// [lex.fcon], [basic.fundamental]: the largest finite and the smallest normal values of float
// and double, spelled in decimal and in hexadecimal, are the same values.
static_assert(1.7976931348623157e308 == 0x1.fffffffffffffp1023);
static_assert(2.2250738585072014e-308 == 0x1p-1022);
static_assert(3.40282347e38f == 0x1.fffffep127f);
static_assert(1.17549435e-38f == 0x1p-126f);
static_assert(0x1.fffffep127f == 0x1.fffffep127);
static_assert(2.220446049250313e-16 == 0x1p-52); // the double epsilon
static_assert(1.0 + 0x1p-52 != 1.0 && 1.0 + 0x1p-53 == 1.0);
static_assert(1.0f + 0x1p-23f != 1.0f && 1.0f + 0x1p-24f == 1.0f);

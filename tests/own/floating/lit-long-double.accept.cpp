// [lex.fcon], [basic.fundamental]: on Apple arm64, long double has the same format as double, so
// an l-suffixed literal has the same value as the unsuffixed one, though not the same type.
// REQUIRES: darwin
static_assert(sizeof(long double) == sizeof(double));
static_assert(0.1L == 0.1);
static_assert(1.7976931348623157e308L == 0x1.fffffffffffffp1023L);
static_assert(0x1p-1074L > 0.0L && 0x1p-1080L == 0.0L);
static_assert(1.0L / 3 == 1.0 / 3);

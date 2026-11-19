// [lex.fcon]: single quotes between digits of a digit-sequence or hexadecimal-digit-sequence, in
// the significand or the exponent, are ignored when determining the value (the draft's example:
// 1.602'176'565e-19 and 1.602176565e-19 have the same value).
static_assert(1.602'176'565e-19 == 1.602176565e-19);
static_assert(1'000.5 == 1000.5);
static_assert(3.141'592'653 == 3.141592653);
static_assert(1e1'0 == 1e10);
static_assert(1'2'3.4'5 == 123.45);
static_assert(0x1'0.8p0 == 16.5);
static_assert(0x1.8'0p1'0 == 0x1.8p10);
static_assert(1'0.f == 10.0f && 2'5e-1L == 2.5L);

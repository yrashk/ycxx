// [lex.fcon], [basic.fundamental]: literals denoting subnormal values keep them: the smallest
// positive double is 2^-1074 and the smallest positive float 2^-149, and decimal spellings round
// to them.
static_assert(0x1p-1074 > 0.0);
static_assert(0x1p-1074 < 0x1p-1022);
static_assert(4.9406564584124654e-324 == 0x1p-1074);
static_assert(5e-324 == 0x1p-1074);
static_assert(0x0.0000000000001p-1022 == 0x1p-1074);
static_assert(0x1p-1073 == 2 * 0x1p-1074);
static_assert(2.2250738585072009e-308 == 0x0.fffffffffffffp-1022); // the largest subnormal
static_assert(0x1p-149f > 0.0f);
static_assert(1.4e-45f == 0x1p-149f);
static_assert(0x1p-149f == 0x1p-149);
static_assert(1.1754942e-38f == 0x0.fffffep-126f);

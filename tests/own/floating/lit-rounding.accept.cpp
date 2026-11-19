// [lex.fcon]: a literal whose scaled value is not representable takes the next greater or lesser
// value of its type (implementation-defined; the target rounds to nearest, ties to even).
static_assert(0.1 == 0x1.999999999999ap-4);
static_assert(0.1f == 0x1.99999ap-4f);
static_assert(0.3 == 0x1.3333333333333p-2);
static_assert(0.1f != 0.1);
static_assert(1.0000000000000001 == 1.0);                    // within half an ulp of 1.0
static_assert(9007199254740993.0 == 9007199254740992.0);     // a tie, to even (down)
static_assert(9007199254740995.0 == 9007199254740996.0);     // a tie, to even (up)
static_assert(16777217.0f == 16777216.0f);                   // a tie in float, to even
static_assert(16777219.0f == 16777220.0f);
static_assert(0x1.0000000000000fp0 == 0x1.0000000000001p0); // 15/16 of an ulp rounds up
static_assert(0x1.00000000000008p0 == 1.0);                  // half an ulp, to even (down)
static_assert(0x1.00000000000018p0 == 0x1.0000000000002p0); // half an ulp, to even (up)

// [lex.fcon]: a literal whose value lies below half the smallest subnormal rounds to zero (the
// next lesser representable value); the literal is still well-formed.
static_assert(1e-400 == 0.0);
static_assert(0x1p-1080 == 0.0);
static_assert(1e-50f == 0.0f);
static_assert(0x1p-160f == 0.0f);
static_assert(2e-324 == 0.0);

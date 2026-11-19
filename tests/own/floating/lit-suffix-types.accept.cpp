// [lex.fcon] (table lex.fcon.type), [over.match.best]: an unsuffixed floating literal has type
// double, f or F gives float, l or L gives long double; each picks its exact-match overload.
constexpr int kind(float) { return 1; }
constexpr int kind(double) { return 2; }
constexpr int kind(long double) { return 3; }
static_assert(kind(1.0) == 2 && kind(1.) == 2 && kind(.5) == 2 && kind(1e3) == 2);
static_assert(kind(1.0f) == 1 && kind(1.0F) == 1 && kind(1.f) == 1 && kind(1e3f) == 1);
static_assert(kind(1.0l) == 3 && kind(1.0L) == 3 && kind(.5L) == 3 && kind(1e3l) == 3);
static_assert(kind(0x1p0) == 2 && kind(0x1p0f) == 1 && kind(0x1p0F) == 1);
static_assert(kind(0x1p0l) == 3 && kind(0x1.8p1L) == 3);
static_assert(sizeof(1.0f) == sizeof(float));
static_assert(sizeof(1.0) == sizeof(double));
static_assert(sizeof(1.0L) == sizeof(long double));
static_assert(sizeof(0x1p0f) == sizeof(float));
static_assert(sizeof(float) <= sizeof(double) && sizeof(double) <= sizeof(long double));

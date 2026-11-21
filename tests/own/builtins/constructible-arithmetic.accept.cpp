// [meta.unary.prop] The invented declaration uses parenthesized direct-initialization.
static_assert(__is_constructible(int, int));
static_assert(__is_constructible(unsigned char, double));
static_assert(__is_constructible(const int, const volatile double&));
static_assert(__is_constructible(double, short&&));
static_assert(__is_constructible(bool, long double));
static_assert(!__is_constructible(int, int, int));
static_assert(!__is_constructible(const int, float&, long&&, bool));

static_assert(__is_same(int, int));
static_assert(__is_trivially_copyable(int));

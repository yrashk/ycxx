struct Owner {};
struct Deferred;
enum class Level : unsigned {};
using Function = long(double);

static_assert(__is_nothrow_constructible(const int));
static_assert(__is_nothrow_constructible(double));
static_assert(__is_nothrow_constructible(Deferred*));
static_assert(__is_nothrow_constructible(decltype(nullptr)));
static_assert(__is_nothrow_constructible(int Owner::*));
static_assert(__is_nothrow_constructible(Level));
static_assert(!__is_nothrow_constructible(int&));
static_assert(!__is_nothrow_constructible(volatile double&&));
static_assert(!__is_nothrow_constructible(Function));
static_assert(!__is_nothrow_constructible(const volatile void));
static_assert(!__is_nothrow_constructible(Function, int));
static_assert(!__is_nothrow_constructible(const volatile void, float));

static_assert(__is_nothrow_constructible(long, short));
static_assert(__is_nothrow_constructible(bool, const volatile double&));
static_assert(__is_nothrow_constructible(float, unsigned&&));
static_assert(!__is_nothrow_constructible(int, short, unsigned));
static_assert(!__is_nothrow_constructible(int, short, unsigned&, double&&));

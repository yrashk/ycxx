struct Owner {};
struct Deferred;
enum class State : unsigned {};
using Function = double(long);

static_assert(__is_trivially_constructible(const volatile int));
static_assert(__is_trivially_constructible(float));
static_assert(__is_trivially_constructible(Deferred*));
static_assert(__is_trivially_constructible(int Owner::*));
static_assert(__is_trivially_constructible(State));
static_assert(__is_trivially_constructible(decltype(nullptr)));

static_assert(!__is_trivially_constructible(int&));
static_assert(!__is_trivially_constructible(const int&&));
static_assert(!__is_trivially_constructible(Function));
static_assert(!__is_trivially_constructible(Function, int));
static_assert(!__is_trivially_constructible(const volatile void));
static_assert(!__is_trivially_constructible(const volatile void, double));

static_assert(__is_trivially_constructible(long, char));
static_assert(__is_trivially_constructible(bool, const volatile double&));
static_assert(__is_trivially_constructible(float, unsigned&&));
static_assert(!__is_trivially_constructible(int, unsigned, double));
static_assert(!__is_trivially_constructible(int, short, int&, float&&));

static_assert(__is_constructible(int, char));
static_assert(__is_nothrow_constructible(int, char));

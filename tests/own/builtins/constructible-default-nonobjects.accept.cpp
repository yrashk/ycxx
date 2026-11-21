// [meta.unary.prop] An argument type may be cv void or an array of unknown bound.
struct Forward;
using Function = int(double);
using Open = const int[][2];

static_assert(!__is_constructible(int&));
static_assert(!__is_constructible(int&&));
static_assert(!__is_constructible(Forward&));
static_assert(!__is_constructible(Function));
static_assert(!__is_constructible(Function, int));
static_assert(!__is_constructible(Function, Open, const volatile void));
static_assert(!__is_constructible(void));
static_assert(!__is_constructible(const volatile void, Open, int));

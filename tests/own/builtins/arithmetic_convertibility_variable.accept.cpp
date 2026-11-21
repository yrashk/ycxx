template<class T>
using Operand = T;

template<class From, class To>
constexpr bool convertible = __is_convertible(Operand<From>, Operand<To>);

template<class From, class To>
constexpr bool nothrow_convertible = __is_nothrow_convertible(Operand<From>, Operand<To>);

using Source = int&&;
using ConstLvalue = const int&;
using VolatileLvalue = const volatile int&;

static_assert(convertible<Source, ConstLvalue>);
static_assert(nothrow_convertible<Source, ConstLvalue>);
static_assert(!convertible<Source, VolatileLvalue>);
static_assert(!nothrow_convertible<Source, VolatileLvalue>);

static_assert(convertible<const void, volatile void>);
static_assert(nothrow_convertible<const void, volatile void>);
static_assert(!convertible<const void, int>);
static_assert(!nothrow_convertible<const void, int>);

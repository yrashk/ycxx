struct Incomplete;
using Function = int(int);
using Bounded = int[3];
using Nested = const volatile double[2][3];
using Unknown = int[];
using UnknownOuter = int[][3];

static_assert(__is_destructible(bool));
static_assert(__is_destructible(int));
static_assert(__is_destructible(const int));
static_assert(__is_destructible(volatile int));
static_assert(__is_destructible(const volatile double));
static_assert(__is_destructible(int* const));
static_assert(__is_destructible(Incomplete*));
static_assert(__is_destructible(decltype(nullptr)));

static_assert(__is_destructible(int&));
static_assert(__is_destructible(volatile int&&));
static_assert(__is_destructible(Incomplete&));
static_assert(__is_destructible(const Incomplete&&));
static_assert(__is_destructible(Function&));
static_assert(__is_destructible(UnknownOuter&));

static_assert(__is_destructible(Bounded));
static_assert(__is_destructible(Nested));

static_assert(!__is_destructible(void));
static_assert(!__is_destructible(const void));
static_assert(!__is_destructible(volatile void));
static_assert(!__is_destructible(const volatile void));
static_assert(!__is_destructible(Function));
static_assert(!__is_destructible(Unknown));
static_assert(!__is_destructible(UnknownOuter));

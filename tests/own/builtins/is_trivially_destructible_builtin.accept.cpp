struct Incomplete;
using Function = void(int);
using Bounded = int[3];
using Nested = const volatile double[2][3];
using Unknown = int[];
using UnknownOuter = int[][3];

static_assert(__is_trivially_destructible(bool));
static_assert(__is_trivially_destructible(const volatile int));
static_assert(__is_trivially_destructible(double));
static_assert(__is_trivially_destructible(int* const));
static_assert(__is_trivially_destructible(Incomplete*));
static_assert(__is_trivially_destructible(decltype(nullptr)));

static_assert(__is_trivially_destructible(int&));
static_assert(__is_trivially_destructible(const Incomplete&));
static_assert(__is_trivially_destructible(Incomplete&&));
static_assert(__is_trivially_destructible(Function&));
static_assert(__is_trivially_destructible(UnknownOuter&));

static_assert(__is_trivially_destructible(Bounded));
static_assert(__is_trivially_destructible(Nested));

static_assert(!__is_trivially_destructible(void));
static_assert(!__is_trivially_destructible(const void));
static_assert(!__is_trivially_destructible(volatile void));
static_assert(!__is_trivially_destructible(const volatile void));
static_assert(!__is_trivially_destructible(Function));
static_assert(!__is_trivially_destructible(Unknown));
static_assert(!__is_trivially_destructible(UnknownOuter));

template<class T>
constexpr bool result = __is_trivially_destructible(T);

static_assert(result<Nested>);
static_assert(result<Incomplete&>);
static_assert(!result<UnknownOuter>);
static_assert(!result<const void>);

template<bool Value>
struct Answer {
    static constexpr bool value = Value;
};

template<class T>
using Operand = T;

template<class T>
struct Query : Answer<__is_trivially_destructible(Operand<T>)> {};

static_assert(Query<Bounded>::value);
static_assert(Query<Function&>::value);
static_assert(!Query<Function>::value);
static_assert(!Query<Unknown>::value);

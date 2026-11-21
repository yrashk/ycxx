template<bool Result>
struct Answer {
    static constexpr bool value = Result;
};

template<class T>
using Operand = T;

template<class T>
struct Query : Answer<__is_destructible(Operand<T>)> {};

struct Incomplete;
using Nested = const int[2][2];
using UnknownOuter = int[][2];
using Function = void();

static_assert(Query<Nested>::value);
static_assert(Query<Incomplete&&>::value);
static_assert(!Query<UnknownOuter>::value);
static_assert(!Query<Function>::value);

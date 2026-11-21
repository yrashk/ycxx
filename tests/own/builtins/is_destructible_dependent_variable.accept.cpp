template<class T>
constexpr bool destructible = __is_destructible(T);

struct Incomplete;
using Bounded = int[2];
using Unknown = int[];

static_assert(destructible<Incomplete&>);
static_assert(destructible<Bounded>);
static_assert(!destructible<Unknown>);
static_assert(!destructible<const void>);

using Result = long;

template<class Source>
using SourceAlias = const Source&;

template<class... Sources>
constexpr bool bare = __is_nothrow_constructible(Result, Sources...);

template<class... Sources>
constexpr bool prefixed = __is_nothrow_constructible(Result, int, SourceAlias<Sources>...);

template<class... Sources>
constexpr bool suffixed = __is_nothrow_constructible(Result, SourceAlias<Sources>..., bool);

template<class... Sources>
constexpr bool surrounded = __is_nothrow_constructible(Result, short, Sources..., float);

template<class... Sources>
constexpr bool constructible = __is_constructible(Result, SourceAlias<Sources>...);

static_assert(bare<>);
static_assert(bare<volatile int&&>);
static_assert(!bare<short, bool>);
static_assert(prefixed<>);
static_assert(!prefixed<double>);
static_assert(suffixed<>);
static_assert(!suffixed<unsigned&, float>);
static_assert(!surrounded<>);
static_assert(!surrounded<double>);
static_assert(constructible<>);
static_assert(constructible<unsigned&>);
static_assert(!constructible<int, float>);

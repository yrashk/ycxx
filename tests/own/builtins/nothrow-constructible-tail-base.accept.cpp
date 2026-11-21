template<bool Value>
struct Verdict {
    static constexpr bool result = Value;
};

template<class... Sources>
struct Bare : Verdict<__is_nothrow_constructible(double, Sources...)> {};

using Result = unsigned;

template<class Source>
using SourceAlias = const Source&;

template<class... Sources>
struct Prefixed : Verdict<__is_nothrow_constructible(Result, short, SourceAlias<Sources>...)> {};

template<class... Sources>
struct Suffixed : Verdict<__is_nothrow_constructible(Result, SourceAlias<Sources>..., long)> {};

template<class... Sources>
using ThroughAlias = Bare<Sources...>;

static_assert(ThroughAlias<>::result);
static_assert(ThroughAlias<const int&>::result);
static_assert(!ThroughAlias<int, float>::result);
static_assert(Prefixed<>::result);
static_assert(!Prefixed<bool>::result);
static_assert(Suffixed<>::result);
static_assert(!Suffixed<bool, const double&>::result);

template<class... Sources>
struct Ordinary : Verdict<__is_constructible(Result, Sources...)> {};

static_assert(Ordinary<>::result);
static_assert(Ordinary<double&&>::result);
static_assert(!Ordinary<int, short>::result);

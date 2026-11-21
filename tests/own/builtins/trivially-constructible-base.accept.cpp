template<bool Value>
struct Verdict {
    static constexpr bool result = Value;
};

template<class Source>
using Borrow = const Source&;

template<class... Sources>
struct Carrier : Verdict<__is_trivially_constructible(long, Borrow<Sources>...)> {};

template<class... Sources>
struct WithSuffix : Verdict<__is_trivially_constructible(long, Borrow<Sources>..., short)> {};

template<class... Sources>
using ThroughAlias = Carrier<Sources...>;

static_assert(ThroughAlias<>::result);
static_assert(ThroughAlias<float>::result);
static_assert(!ThroughAlias<unsigned, char>::result);
static_assert(WithSuffix<>::result);
static_assert(!WithSuffix<double>::result);

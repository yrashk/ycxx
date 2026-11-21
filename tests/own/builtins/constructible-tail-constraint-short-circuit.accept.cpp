struct Incomplete;

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...);

template<bool Demand, class Target, class... Args>
  requires (Demand && constructs<Target, Args...>)
constexpr int choose() { return 1; }

template<bool Demand, class Target, class... Args>
constexpr int choose() { return 2; }

static_assert(choose<false, void, Incomplete>() == 2);
static_assert(choose<true, int, double&>() == 1);

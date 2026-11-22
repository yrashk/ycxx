struct Incomplete;

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...);

template<bool Demand, class Argument>
constexpr int choose(Argument*) { return 2; }

template<bool Demand, class Argument>
  requires (Demand && constructs<int, Argument>)
constexpr int choose(Argument*) { return 1; }

static_assert(choose<false>(static_cast<Incomplete*>(nullptr)) == 2);
static_assert(choose<true>(static_cast<double*>(nullptr)) == 1);

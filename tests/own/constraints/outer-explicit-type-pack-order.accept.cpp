template<class Target, class... Args>
constexpr bool matches = false;

template<>
constexpr bool matches<int, double&, const char*> = true;

template<class Target, class... Args>
  requires matches<Target, Args...>
constexpr int choose() { return 1; }

template<class Target, class... Args>
constexpr int choose() { return 2; }

static_assert(choose<int, double&, const char*>() == 1);
static_assert(choose<int, const char*, double&>() == 2);
static_assert(choose<int, double, const char*>() == 2);
static_assert(choose<int, double&>() == 2);
static_assert(choose<int>() == 2);

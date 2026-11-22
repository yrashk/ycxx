template<class Target, class... Args>
constexpr bool yes = true;

template<class Target, class... Args>
constexpr bool no = false;

template<class Target, class... Args>
  requires yes<Target, Args...>
constexpr int choose_true() { return 1; }

template<class Target, class... Args>
constexpr int choose_true() { return 2; }

template<class Target, class... Args>
  requires no<Target, Args...>
constexpr int choose_false() { return 1; }

template<class Target, class... Args>
constexpr int choose_false() { return 2; }

static_assert(choose_true<int>() == 1);
static_assert(choose_true<int, double&>() == 1);
static_assert(choose_true<int, double&, const char*>() == 1);
static_assert(choose_false<int>() == 2);
static_assert(choose_false<int, double&>() == 2);
static_assert(choose_false<int, double&, const char*>() == 2);

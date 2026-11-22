template<class Target, class Arg>
constexpr bool yes = true;

template<class Target, class Arg>
constexpr bool no = false;

template<class Target, class Arg>
  requires yes<Target, Arg>
constexpr int choose_true() { return 1; }

template<class Target, class Arg>
constexpr int choose_true() { return 2; }

template<class Target, class Arg>
  requires no<Target, Arg>
constexpr int choose_false() { return 1; }

template<class Target, class Arg>
constexpr int choose_false() { return 2; }

static_assert(choose_true<int, double&>() == 1);
static_assert(choose_false<int, double&>() == 2);

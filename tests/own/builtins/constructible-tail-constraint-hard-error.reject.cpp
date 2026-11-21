struct Incomplete;

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...); //! error

template<class Target, class... Args>
  requires (constructs<Target, Args...>)
int choose();

template<class Target, class... Args>
int choose();

int demand() { return choose<void, Incomplete>(); }

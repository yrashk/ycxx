struct Incomplete;

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...); //! error

template<class Argument>
int choose(Argument*);

template<class Argument>
  requires (constructs<void, Argument>)
int choose(Argument*);

int demand(Incomplete* pointer) { return choose(pointer); }

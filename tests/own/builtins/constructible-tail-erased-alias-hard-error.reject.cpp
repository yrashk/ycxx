struct Incomplete;
template<bool> struct Flag {};
template<bool> using Erase = Flag<true>;

template<class Target, class... Args>
struct Constructs : Erase<__is_constructible(Target, Args...)> {}; //! error

static_assert(sizeof(Constructs<void, Incomplete>) > 0);

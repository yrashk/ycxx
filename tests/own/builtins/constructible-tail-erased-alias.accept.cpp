template<bool> struct Flag {};
template<bool> using Erase = Flag<true>;

template<class Target, class... Args>
struct Constructs : Erase<__is_constructible(Target, Args...)> {};

Flag<true>* false_value_is_erased(Constructs<int, short, long>* p) { return p; }

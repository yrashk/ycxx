struct Incomplete;
template<bool> struct Flag {};
template<bool> using Erase = Flag<true>;

template<class Target, class... Args>
struct Constructs : Erase<__is_constructible(Target, Args...)> {}; //! error

Flag<true>* demand(Constructs<void, Incomplete>* p) { return p; }

// XFAIL: gcc GCC also reports a follow-up conversion error at the demand site.

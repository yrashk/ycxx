template<bool> struct Flag {};

template<class Target, class... Args>
struct Constructs : Flag<__is_constructible(Target, Args...)> {};

Flag<true>* empty(Constructs<int>* p) { return p; }
Flag<true>* one(Constructs<int, double&>* p) { return p; }
Flag<false>* many(Constructs<int, short, long>* p) { return p; }

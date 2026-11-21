struct Later;

template<bool> struct Flag {};

template<class Target, class... Args>
struct Constructs : Flag<__is_constructible(Target, Args...)> {};

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...);

struct Later { int value; };

Flag<false>* false_base(Constructs<void, Later>* p) { return p; }
static_assert(!constructs<void, Later>);

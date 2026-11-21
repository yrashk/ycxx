template<class T> constexpr bool copied = __is_trivially_copyable(T); //! error

template<bool Demand, class T> constexpr int choose(T*) { return 1; }
template<bool Demand, class T> requires (Demand && copied<T>)
constexpr int choose(T*) { return 2; }

struct Forward;
constexpr int selected = choose<true>(static_cast<Forward*>(nullptr));

template<class T> constexpr bool copied = __is_trivially_copyable(T);

template<bool Demand, class T> constexpr int choose(T*) { return 1; }
template<bool Demand, class T> requires (Demand && copied<T>)
constexpr int choose(T*) { return 2; }

struct Forward;
static_assert(choose<false>(static_cast<Forward*>(nullptr)) == 1);
static_assert(choose<true>(static_cast<int*>(nullptr)) == 2);

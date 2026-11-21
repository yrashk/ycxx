struct Empty;

template<class T>
constexpr bool checked = __is_empty(T); //! error

template<class T>
constexpr bool mirror = checked<T>;

bool demand = mirror<Empty>;

struct Empty {};

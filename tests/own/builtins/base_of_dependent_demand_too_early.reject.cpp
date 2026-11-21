struct B {};
struct D;

template<class T>
constexpr bool v = __is_base_of(T, D); //! error

bool demand = v<B>;

struct D : B {};

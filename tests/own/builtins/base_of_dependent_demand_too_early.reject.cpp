struct B {};
struct D;

template<class T>
constexpr bool v = __is_base_of(T, D); //! error

static_assert(v<B>);

struct D : B {};

struct B {};
struct D;

template<class T>
constexpr bool v = __is_base_of(T, D);

static_assert(v<const D>);
static_assert(!v<int>);

struct D : private B {};

static_assert(v<B>);

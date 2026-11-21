struct Empty;
struct Nonempty;

template<class T>
using Operand = T;

template<class T>
constexpr bool empty = __is_empty(Operand<T>);

struct Empty {};
struct Nonempty {
    int value;
};

static_assert(empty<Empty>);
static_assert(!empty<Nonempty>);

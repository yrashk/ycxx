struct Empty;

template<class T>
using Operand = T;

template<class T>
constexpr bool empty = __is_empty(Operand<T>);

using Early = decltype(empty<Empty>);
static_assert(sizeof(Early) == sizeof(bool));

struct Empty {};

static_assert(empty<Empty>);

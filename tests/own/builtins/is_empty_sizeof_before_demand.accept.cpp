struct Empty;

template<class T>
using Operand = T;

template<class T>
constexpr bool empty = __is_empty(Operand<T>);

static_assert(sizeof(empty<Empty>) == sizeof(bool));

struct Empty {};

static_assert(empty<Empty>);

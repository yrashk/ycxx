struct Empty;

template<class T>
using Operand = T;

template<class T>
constexpr bool empty = __is_empty(Operand<T>);

template<bool Read, class T>
constexpr bool query() {
    if constexpr (Read)
        return empty<T>;
    return false;
}

static_assert(!query<false, Empty>());

struct Empty {};

static_assert(query<true, Empty>());

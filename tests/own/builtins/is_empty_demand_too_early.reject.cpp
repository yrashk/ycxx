struct Incomplete;

template<class T>
using Operand = T;

template<class T>
constexpr bool empty = __is_empty(Operand<T>); //! error

bool query() {
    return empty<Incomplete>;
}

struct Incomplete {};

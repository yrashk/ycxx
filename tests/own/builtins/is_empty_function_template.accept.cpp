struct Empty {};

template<class T>
constexpr bool empty() {
    return __is_empty(T);
}

static_assert(empty<Empty>());

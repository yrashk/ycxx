template<class T>
struct IsLongDouble {
    static constexpr bool value = false;
};

template<>
struct IsLongDouble<long double> {
    static constexpr bool value = true;
};

template<class T>
auto fabsl_result(T input) -> decltype(__builtin_fabsl(input));

static_assert(IsLongDouble<decltype(fabsl_result(7))>::value);

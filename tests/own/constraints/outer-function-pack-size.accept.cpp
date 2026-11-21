template<class... T>
constexpr int choose(T... xs) requires (sizeof...(xs) != 0) { return 1; }
template<class... T>
constexpr int choose(T...) { return 2; }
static_assert(choose<>() == 2);
static_assert(choose<int>(1) == 1);
static_assert(choose<int>(1, 2.0) == 1);

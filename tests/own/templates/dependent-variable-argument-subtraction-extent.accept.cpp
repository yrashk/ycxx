// #1091: match each recursive extent specialization using the converted value of I - 1
// ([temp.arg.nontype], [temp.inst], [temp.spec.partial.match], [temp.spec.partial.order]).
using Size = decltype(sizeof(0));

template<class T, unsigned I = 0>
inline constexpr Size extent = 0;
template<class T, Size N>
inline constexpr Size extent<T[N], 0> = N;
template<class T, unsigned I>
inline constexpr Size extent<T[], I> = extent<T, I - 1>;
template<class T, Size N, unsigned I>
inline constexpr Size extent<T[N], I> = extent<T, I - 1>;

static_assert(extent<int[2][3][5]> == 2);
static_assert(extent<int[2][3][5], 1> == 3);
static_assert(extent<int[2][3][5], 2> == 5);
static_assert(extent<int[][3][5], 0> == 0);
static_assert(extent<int[][3][5], 1> == 3);
static_assert(extent<int[][3][5], 2> == 5);
static_assert(extent<int[2][3][5], 3> == 0);

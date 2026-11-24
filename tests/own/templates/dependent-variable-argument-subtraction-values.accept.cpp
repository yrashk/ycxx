// #1091: subtraction in a value argument is evaluated after substitution. Unsigned
// arithmetic wraps ([basic.fundamental], [expr.add], [temp.arg.nontype], [temp.dep.constexpr]).
template<unsigned I>
constexpr unsigned index = I;
template<unsigned I>
constexpr unsigned previous = index<I - 1>;

static_assert(previous<4> == 3);
static_assert(previous<1> == 0);
static_assert(previous<0> == ~0u);

template<int I>
constexpr int signed_index = I;
template<class T, int I>
constexpr int next = 7;
template<class T, int I>
constexpr int next<T*, I> = signed_index<I - (-1)>;

static_assert(next<int*, 3> == 4);
// The unrestricted primary is selected, so the partial's overflowing argument is unused.
static_assert(next<int, __INT_MAX__> == 7);

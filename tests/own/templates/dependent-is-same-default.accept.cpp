// Substitute the dependent default only if the argument is omitted ([temp.param], #209).
template<bool B> struct Bool { static constexpr bool value = B; };

template<class T, class U, bool B = __is_same(T, U)>
using EqualByDefault = Bool<B>;

static_assert(EqualByDefault<int, int>::value);
static_assert(!EqualByDefault<int, long>::value);
static_assert(!EqualByDefault<int, int, false>::value);
static_assert(EqualByDefault<int, long, true>::value);

// #209 [temp.dep.constexpr], [temp.arg.nontype], [temp.variadic], [temp.alias].
template<int Number>
struct Rank {};

template<class First, class... Rest>
consteval int shape_rank() { return sizeof(First) * 10 + sizeof...(Rest); }

template<class Head, class... Tail>
using Shape = Rank<shape_rank<Head, Tail...>()>;

static_assert(__is_same(Rank<shape_rank<char[4], bool, char>()>, Rank<42>));
static_assert(__is_same(Shape<char[2]>, Rank<20>));
static_assert(__is_same(Shape<char[2], char, int>, Rank<22>));
static_assert(__is_same(Shape<char[4], bool, char>, Rank<42>));

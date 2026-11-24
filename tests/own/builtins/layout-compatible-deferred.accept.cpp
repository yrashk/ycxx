// #218: dependent `__is_layout_compatible` operands in a class-template argument and a
// variable-template initializer, as libycxx's <type_traits> has them, are decided at
// instantiation, with completeness judged there ([temp.point]).
template <bool B> struct Answer { static constexpr bool value = B; };
template <class T, class U>
struct is_layout_compatible : Answer<__is_layout_compatible(T, U)> {};
template <class T, class U>
inline constexpr bool is_layout_compatible_v = __is_layout_compatible(T, U);

struct S1 { int a; char b; };
struct Late;
static_assert(is_layout_compatible<int, const int>::value && !is_layout_compatible_v<int, long>);
struct Late { int x; char y; };
static_assert(is_layout_compatible<S1, Late>::value && is_layout_compatible_v<Late, const S1>);
static_assert(!is_layout_compatible<S1, int>::value && is_layout_compatible_v<Late, Late>);

union U1 { int a; char b; };
union U2 { char c; int d; };
enum EA : int {};
enum class EC : int {};
enum ED : long {};
static_assert(is_layout_compatible<U1, U2>::value && !is_layout_compatible_v<U1, Late>);
static_assert(is_layout_compatible<EA, EC>::value && !is_layout_compatible_v<EA, ED>);

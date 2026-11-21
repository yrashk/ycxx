// Substitution keeps the second operand dependent and preserves cv and reference identity (#209).
template<bool B> struct Bool { static constexpr bool value = B; };
template<class T, class U> using IsSame = Bool<(__is_same(T, U))>;

using Word = long;
using ReadOnlyWord = const Word;
template<class U> using IsInt = IsSame<int, U>;
template<class U> using IsConstInt = IsSame<const int, U>;

static_assert(IsInt<int>::value);
static_assert(!IsInt<const int>::value);
static_assert(IsConstInt<const int>::value);
static_assert(!IsConstInt<int>::value);
static_assert(IsSame<ReadOnlyWord, long const>::value);
static_assert(IsSame<ReadOnlyWord&, long const&>::value);
static_assert(!IsSame<ReadOnlyWord, Word>::value);
static_assert(!IsSame<ReadOnlyWord&, Word&>::value);
static_assert(!IsSame<Word&, Word&&>::value);

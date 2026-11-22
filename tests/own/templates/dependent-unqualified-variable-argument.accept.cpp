template<bool, class T, class U> struct ConditionalImpl { using type = T; };
template<class T, class U> struct ConditionalImpl<false, T, U> { using type = U; };
template<bool B, class T, class U> using Conditional = typename ConditionalImpl<B, T, U>::type;
template<class T> inline constexpr bool IsConst = __is_const(T);
template<class T, class U> using CopyConst = Conditional<IsConst<T>, const U, U>;
static_assert(__is_same(CopyConst<const int, long>, const long));
static_assert(__is_same(CopyConst<int, long>, long));

template<class T> using Parenthesized = Conditional<(IsConst<T>), int, long>;
static_assert(__is_same(Parenthesized<const int>, int));
static_assert(__is_same(Parenthesized<int>, long));

namespace qualified {
template<class T> inline constexpr bool IsConst = __is_const(T);
}
template<class T> using Qualified = Conditional<qualified::IsConst<T>, int, long>;
static_assert(__is_same(Qualified<const int>, int));
static_assert(__is_same(Qualified<int>, long));

namespace local_lookup {
template<class> inline constexpr bool IsConst = false;
template<class T> using Choice = Conditional<IsConst<T>, int, long>;
}
static_assert(__is_same(local_lookup::Choice<const int>, long));

namespace hides_variable_template {
template<class T> inline constexpr bool Flag = true;
template<bool B> int choose() { return B ? 1 : 2; }
template<class T> int block() {
    constexpr int Flag = 2;
    return choose<(Flag < 2)>();
}
template<class T> struct Enclosing {
    static constexpr int Flag = 2;
    static int evaluate() { return choose<(Flag < 2)>(); }
};
int use() { return block<int>() + Enclosing<int>::evaluate(); }
}

namespace dependent_adl {
template<class T> int pick(int);
template<class T> int call(T& value) { return pick<T>(value); }
namespace associated {
struct Value {};
template<class T> int pick(Value&) { return 9; }
}
int use() {
    associated::Value value;
    return call(value);
}
}

namespace same_entity {
namespace first { template<class> inline constexpr bool Flag = false; }
namespace second { using first::Flag; }
using namespace first;
using namespace second;
template<class T> using Choice = Conditional<Flag<T>, int, long>;
static_assert(__is_same(Choice<int>, long));
}

template<class> inline constexpr bool IsLRef = false;
template<class T> inline constexpr bool IsLRef<T&> = true;
template<class T> using RefChoice = Conditional<IsLRef<T&&>, int, long>;
static_assert(__is_same(RefChoice<int&>, int));
static_assert(__is_same(RefChoice<int>, long));

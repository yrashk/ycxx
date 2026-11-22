template<bool, class Yes, class No> struct Select { using Type = No; };
template<class Yes, class No> struct Select<true, Yes, No> { using Type = Yes; };
template<bool Test, class Yes, class No>
using Conditional = typename Select<Test, Yes, No>::Type;

template<class A, class B> constexpr bool Safe = false;
namespace traits {
template<class A, class B> constexpr bool Copy = true;
}

template<class T> using Combined = Conditional<!Safe<T, T&&> && traits::Copy<T, const T&>, const T&, T&&>;
template<class T> using OneOperand = Conditional<traits::Copy<T, T&&> && true, const T&, T&&>;
template<class T> using LiteralFirst = Conditional<true && Safe<T, T&&>, const T&, T&&>;
static_assert(__is_same(Combined<int>, const int&));
static_assert(__is_same(OneOperand<int>, const int&));
static_assert(__is_same(LiteralFirst<int>, int&&));

template<class T> constexpr bool Enabled = false;
template<class T> bool Runtime = true;
template<class T> using Skipped = Conditional<Enabled<T> && Runtime<T>, const T&, T&&>;
static_assert(__is_same(Skipped<int>, int&&));

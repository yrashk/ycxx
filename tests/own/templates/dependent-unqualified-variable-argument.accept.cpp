template<bool, class T, class U> struct ConditionalImpl { using type = T; };
template<class T, class U> struct ConditionalImpl<false, T, U> { using type = U; };
template<bool B, class T, class U> using Conditional = typename ConditionalImpl<B, T, U>::type;
template<class T> inline constexpr bool IsConst = __is_const(T);
template<class T, class U> using CopyConst = Conditional<IsConst<T>, const U, U>;
static_assert(__is_same(CopyConst<const int, long>, const long));
static_assert(__is_same(CopyConst<int, long>, long));

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

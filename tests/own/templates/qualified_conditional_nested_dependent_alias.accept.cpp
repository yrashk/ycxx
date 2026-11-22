namespace mini {
inline namespace v1 {

template<bool Condition, class WhenTrue, class WhenFalse>
struct Select {
    using Type = WhenTrue;
};

template<class WhenTrue, class WhenFalse>
struct Select<false, WhenTrue, WhenFalse> {
    using Type = WhenFalse;
};

template<bool Condition, class WhenTrue, class WhenFalse>
using Conditional = typename Select<Condition, WhenTrue, WhenFalse>::Type;

template<class T>
constexpr bool IsConst = false;

template<class T>
constexpr bool IsConst<const T> = true;

template<class T>
struct RemoveRefImpl {
    using Type = T;
};

template<class T>
struct RemoveRefImpl<T&> {
    using Type = T;
};

template<class T>
struct RemoveRefImpl<T&&> {
    using Type = T;
};

template<class T>
using RemoveRef = typename RemoveRefImpl<T>::Type;

} // namespace v1
} // namespace mini

template<class T, class U>
using ForwardBase =
    mini::Conditional<mini::IsConst<mini::RemoveRef<T>>, const mini::RemoveRef<U>, mini::RemoveRef<U>>;

static_assert(__is_same(ForwardBase<const int&, long&>, const long));
static_assert(__is_same(ForwardBase<int&, long&>, long));
static_assert(__is_same(ForwardBase<const int&&, long&&>, const long));
static_assert(__is_same(ForwardBase<int&&, long&&>, long));
static_assert(__is_same(ForwardBase<const volatile int&, volatile long&>, const volatile long));

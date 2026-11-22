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

template<class U>
using ConstBranch = mini::Conditional<true, const mini::RemoveRef<U>, mini::RemoveRef<U>>;

template<class U>
using PlainBranch =
    mini::Conditional<mini::IsConst<int>, const mini::RemoveRef<U>, mini::RemoveRef<U>>;

static_assert(__is_same(ConstBranch<long&>, const long));
static_assert(__is_same(PlainBranch<long&&>, long));

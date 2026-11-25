// #1178, [temp.deduct], [expr.prim.req], [basic.lookup.argdep]: the validity of an operator
// expression on dependent operands depends on the operands' hidden friends.
// EXIT: 0
template <class T> concept CanInvert = requires(const T& t) { ~t; };
template <class T> concept CanInc = requires(T& t) { { ++t } noexcept; };
template <class T, class U> concept CanAdd = requires(T& t, const U& u) { t += u; };

namespace n {
struct None {};
struct Some {
    friend constexpr int operator~(const Some&) { return 1; }
    friend Some& operator++(Some&) noexcept;
    friend Some& operator+=(Some&, int);
    friend Some& operator*=(Some&, const Some&);
    friend int operator*(Some);
};
struct Gone {
    friend int operator~(const Gone&) = delete;
    friend Gone& operator++(Gone&);
    friend void operator+=(Gone&, const Gone&) = delete;
    friend void operator+=(Gone&, long);
    friend int operator*=(const Gone&, int);
};
struct Outer {
    enum class In {};
    friend In operator~(In);
    friend int operator++(In&) noexcept;
    enum class Out {};
};
}

template <class T, class = void> constexpr bool derefs = false;
template <class T> constexpr bool derefs<T, decltype(void(*T()))> = true;

static_assert(!CanInvert<n::None> && CanInvert<n::Some> && !CanInvert<n::Gone>);
static_assert(CanInvert<n::Outer::In> && !CanInvert<n::Outer::Out>);
static_assert(!CanInc<n::None> && CanInc<n::Some> && !CanInc<n::Gone> && CanInc<n::Outer::In>);
static_assert(!CanInc<const n::Some> && !CanInc<const n::Outer::In>);
static_assert(!CanAdd<n::None, int> && CanAdd<n::Some, int> && CanAdd<n::Some, char>);
static_assert(!CanAdd<const n::Some, int> && !CanAdd<n::Some, n::Some>);
static_assert(!CanAdd<n::Gone, n::Gone> && CanAdd<n::Gone, int>);
static_assert(!derefs<n::None> && derefs<n::Some> && !derefs<n::Gone>);

int main() {
    n::None none;
    n::Some some;
    n::Gone gone;
}

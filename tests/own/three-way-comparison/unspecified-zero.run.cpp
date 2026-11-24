// [cmp.categories.pre]/3: a comparison category's operators may take the literal `0` as an
// "unspecified" parameter of pointer type, as standard libraries declare them. The `0` of a
// rewritten `(x <=> y) @ 0` or `0 @ (y <=> x)` ([over.match.oper]/8) and of a defaulted `<=>`'s
// `vi != 0` ([class.spaceship]/3) is then a null pointer constant.
namespace std {
struct Unspecified {
    consteval Unspecified(Unspecified*) noexcept {}
};
class strong_ordering {
    signed char v_;
    constexpr explicit strong_ordering(signed char v) noexcept : v_(v) {}

public:
    static const strong_ordering less, equal, equivalent, greater;
    friend constexpr bool operator==(strong_ordering a, Unspecified) noexcept { return a.v_ == 0; }
    friend constexpr bool operator<(strong_ordering a, Unspecified) noexcept { return a.v_ < 0; }
    friend constexpr bool operator>(strong_ordering a, Unspecified) noexcept { return a.v_ > 0; }
    friend constexpr bool operator<(Unspecified, strong_ordering a) noexcept { return 0 < a.v_; }
    friend constexpr bool operator>(Unspecified, strong_ordering a) noexcept { return 0 > a.v_; }
    friend constexpr strong_ordering operator<=>(Unspecified, strong_ordering a) noexcept {
        return strong_ordering(static_cast<signed char>(-a.v_));
    }
};
inline constexpr strong_ordering strong_ordering::less(-1);
inline constexpr strong_ordering strong_ordering::equal(0);
inline constexpr strong_ordering strong_ordering::equivalent(0);
inline constexpr strong_ordering strong_ordering::greater(1);
}  // namespace std

struct W {
    int v;
    friend constexpr std::strong_ordering operator<=>(W w, int i) { return w.v <=> i; }
};
static_assert(W{1} < 2 && W{3} > 2);   // (w <=> i) @ 0
static_assert(2 < W{3} && 2 > W{1});   // 0 @ (w <=> i)
static_assert((2 <=> W{3}) < 0);       // 0 <=> (w <=> i)

struct P {
    int x, y;
    auto operator<=>(const P&) const = default;
};
static_assert((P{1, 2} <=> P{1, 3}) < 0 && P{2, 0} > P{1, 3});

int main() {
    P a{1, 2}, b{1, 3};
    W w{5};
    return a < b && 4 < w && !(w < 5) ? 0 : 1;
}

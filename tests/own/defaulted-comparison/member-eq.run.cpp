// [class.compare.default], [class.eq]: a defaulted equality operator declared as a member.
struct P {
    int x, y;
    bool operator==(const P&) const = default;
};

struct Q {
    int x;
    P p;
    constexpr bool operator==(const Q&) const = default;
};

static_assert(noexcept(P{1, 2} == P{1, 2}));
static_assert(P{1, 2} == P{1, 2});
static_assert(P{1, 2} != P{1, 3});

int main() {
    P a{1, 2}, b{1, 2}, c{1, 3};
    Q q{1, {2, 3}}, r{1, {2, 3}}, s{1, {2, 4}};  // at run time: #1140
    return (a == b && !(a != b) && a != c && !(a == c) && q == r && q != s) ? 0 : 1;
}

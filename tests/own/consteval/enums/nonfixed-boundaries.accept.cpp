// [dcl.enum], [expr.static.cast]: a non-fixed enum admits unnamed values within its range.
enum Signed { negative = -3, positive = 2 };

constexpr Signed as_signed(long long value) { return static_cast<Signed>(value); }
constexpr Signed values[] = {as_signed(-4), as_signed(3)};
static_assert(static_cast<int>(values[0]) == -4);
static_assert(static_cast<int>(values[1]) == 3);

enum Ordinary { largest = 5 };
constexpr bool check_local() {
    constexpr Ordinary value = static_cast<Ordinary>(7u);
    static_assert(static_cast<unsigned>(value) == 7u);
    return value != largest;
}

static_assert(check_local());

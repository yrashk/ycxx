// [class.spaceship]/3, [except.spec]/11
// The nonzero test uses an lvalue. Copying the selected result can throw.
#include "inputs/compare.hpp"

struct R {
    int payload;

    R(std::strong_ordering order) noexcept : payload(order < 0 ? -1 : order > 0 ? 1 : 0) {}
    R(const R&) noexcept(false) { throw 42; }

    bool operator!=(int zero) & noexcept { return payload != zero; }
    bool operator!=(int) && noexcept(false) { throw 37; }
    friend bool operator==(R result, int zero) noexcept { return result.payload == zero; }
};

struct B {
    int field;
    R operator<=>(const B&) const = default;
};

static_assert(!noexcept(B{1} <=> B{2}));
static_assert(!noexcept(R(std::strong_ordering::equal) != 0));

int main() {
    R zero = std::strong_ordering::equal;
    static_assert(noexcept(zero != 0));

    try {
        (void)(B{1} <=> B{2});
    } catch (int error) {
        return error == 42 ? 0 : 1;
    }
    return 2;
}

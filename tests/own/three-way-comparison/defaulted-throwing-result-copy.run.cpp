// [class.spaceship]/3, [except.spec]/11
// The nonzero test uses an lvalue. Copying the selected result can throw.
#include "inputs/compare.hpp"

int copies = 0;
int rvalue_tests = 0;

struct R {
    int payload;

    R(std::strong_ordering order) noexcept : payload(order < 0 ? -1 : order > 0 ? 1 : 0) {}
    R(const R& source) noexcept(false) : payload(source.payload) { ++copies; }

    bool operator!=(int zero) & noexcept { return payload != zero; }
    bool operator!=(int zero) && noexcept(false) {
        ++rvalue_tests;
        return payload != zero;
    }
    friend bool operator==(R result, int zero) noexcept { return result.payload == zero; }
};

struct B {
    int field;
    R operator<=>(const B&) const = default;
};

static_assert(!noexcept(B{1} <=> B{2}));
static_assert(!noexcept(R(std::strong_ordering::equal) != 0));

int main(int argc, char**) {
    R zero = std::strong_ordering::equal;
    static_assert(noexcept(zero != 0));

    R result = B{argc} <=> B{argc + 1};
    return result.payload != -1 || copies == 0 || rvalue_tests != 0;
}

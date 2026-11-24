// [expr.const.imm], [expr.const.core]
#include "checked-sum.hpp"

consteval int checked_right(int value) {
    if (value < 0) {
        auto identity = [](int result) { return result; };
        return identity(checked_sum<2>(value) ? value : 0);
    }
    return value;
}

constexpr int right_choice(int value) {
    if consteval {
        return checked_right(value);
    } else {
        return value + 20;
    }
}

static_assert(right_choice(3) == 3);

int right_runtime(int value) {
    return right_choice(value);
}

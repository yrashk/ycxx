// [expr.const.imm], [expr.const.core]
#include "checked-sum.hpp"

consteval int checked_left(int value) {
    if (value < 0) {
        auto identity = [](int result) { return result; };
        return identity(checked_sum<2>(value) ? value : 0);
    }
    return value;
}

constexpr int left_choice(int value) {
    if consteval {
        return checked_left(value);
    } else {
        return value + 10;
    }
}

static_assert(left_choice(2) == 2);

int right_runtime(int value);

int main(int argc, char**) {
    return left_choice(argc) == argc + 10 && right_runtime(argc) == argc + 20 ? 0 : 1;
}

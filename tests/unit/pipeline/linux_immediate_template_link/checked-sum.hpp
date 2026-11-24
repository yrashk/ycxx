#pragma once

using Intmax = __INTMAX_TYPE__;

consteval Intmax checked_input(Intmax value) {
    return value + 1;
}

template<int Increment>
consteval bool checked_sum(Intmax value) {
    Intmax result;
    return !__builtin_add_overflow(checked_input(value), Intmax{Increment}, &result) &&
           result == value + 1 + Increment;
}

static_assert(checked_sum<1>(40));
static_assert(checked_sum<2>(-1));

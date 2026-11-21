#ifndef HIDDEN_HPP
#define HIDDEN_HPP

[[gnu::visibility("hidden")]] inline int hidden_step(int value) {
    return value + 1;
}

[[nodiscard]] [[maybe_unused, __gnu__::__visibility__("hidden")]]
constexpr int hidden_twice(int value) {
    return value * 2;
}

#endif

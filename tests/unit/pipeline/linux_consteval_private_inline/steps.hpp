#ifndef YCXX_TEST_CONSTEVAL_PRIVATE_INLINE_STEPS_HPP
#define YCXX_TEST_CONSTEVAL_PRIVATE_INLINE_STEPS_HPP

using Step = int (*)(int);

class Steps {
    static constexpr int add(int value) { return value + 9; }
    static constexpr int twice(int value) { return value * 2; }

public:
    static constexpr int run(int value) {
        if consteval {
            return value;
        } else {
            return add(value);
        }
    }

    static constexpr Step address() { return &Steps::twice; }
};

#endif

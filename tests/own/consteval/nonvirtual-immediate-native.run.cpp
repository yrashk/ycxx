// [expr.const.imm], [expr.const.core], [dcl.constexpr]
int immediate_fallback(int input) { return input - 100; }
int runtime_fallback(int input) { return input + 10; }

struct Offset {
    int base;

    consteval int immediate_member(int input) const {
        if (input < 0)
            return immediate_fallback(input) + base;
        return base + input;
    }

    constexpr int runtime_member(int input) const {
        if (input < 0)
            return runtime_fallback(input) + base;
        return base + input;
    }
};

template<int Base>
consteval int immediate_template(int input) {
    if (input < 0)
        return immediate_fallback(input) + Base;
    return Base + input;
}

template<int Base>
constexpr int runtime_template(int input) {
    if (input < 0)
        return runtime_fallback(input) + Base;
    return Base + input;
}

int main() {
    constexpr Offset offset{9};
    int member = offset.immediate_member(5);
    int specialization = immediate_template<20>(3);
    if (member != 14)
        return 1;
    if (specialization != 23)
        return 2;

    volatile int input = -4;
    if (offset.runtime_member(input) != 15)
        return 3;
    if (runtime_template<20>(input) != 26)
        return 4;
    return 0;
}

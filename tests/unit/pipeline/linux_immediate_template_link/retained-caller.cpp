// [expr.const.imm], [expr.const.core]
template<class T>
consteval int shift(T value) {
    return int(value) + 4;
}

struct Offset {
    static consteval int add(int value) {
        return shift(long(value)) + 3;
    }
};

consteval int wrapper(int value) {
    if (value < 0) {
        auto identity = [](int n) { return n; };
        return identity(Offset::add(value));
    }
    return value;
}

int main() {
    return wrapper(42) - 42;
}

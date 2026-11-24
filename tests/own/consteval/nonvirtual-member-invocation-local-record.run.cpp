// [expr.const.core], [expr.const.init], [basic.life]
struct Record {
    int value;
    mutable int cache;

    consteval bool aliases(const Record& other) const {
        const Record& self = *this;
        return &self == &other;
    }

    consteval int read_value() const {
        return value;
    }
};

int main() {
    constexpr Record first{42, 7};
    constexpr Record second{42, 7};

    if (first.aliases(second))
        return 1;
    if (!first.aliases(first))
        return 2;
    if (first.read_value() != 42)
        return 3;
    return 0;
}

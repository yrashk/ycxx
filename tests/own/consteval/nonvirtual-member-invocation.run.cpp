// [expr.const.imm], [expr.call]
struct Gauge {
    int bias;

    consteval int adjusted(int delta) const {
        return this->bias + delta;
    }
};

consteval int adjusted_in_context(int bias, int delta) {
    Gauge gauge{bias};
    return gauge.adjusted(delta);
}

int main() {
    constexpr Gauge gauge{40};
    if (gauge.adjusted(2) != 42)
        return 1;
    if (adjusted_in_context(30, 5) != 35)
        return 2;
    return 0;
}

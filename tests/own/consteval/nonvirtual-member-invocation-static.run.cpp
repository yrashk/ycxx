// [expr.const.imm], [expr.call], [expr.ref]
struct Gauge {
    int bias;

    static consteval int doubled(int value) {
        return 2 * value;
    }
};

consteval int doubled_through_object(int bias, int value) {
    Gauge gauge{0};
    int doubled = (gauge.bias = bias, gauge).doubled(value);
    return doubled + gauge.bias;
}

int main() {
    if (Gauge::doubled(21) != 42)
        return 1;
    if (doubled_through_object(8, 4) != 16)
        return 2;
    return 0;
}

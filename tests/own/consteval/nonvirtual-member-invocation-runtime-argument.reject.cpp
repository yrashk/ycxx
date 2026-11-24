// [expr.const.imm], [expr.const.core], [expr.call]
struct Gauge {
    int bias;

    consteval int adjusted(int delta) const {
        return this->bias + delta;
    }
};

int runtime_argument(int delta) {
    constexpr Gauge gauge{40};
    return gauge.adjusted(delta); //! error
}

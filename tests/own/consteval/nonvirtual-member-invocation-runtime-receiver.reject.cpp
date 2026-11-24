// [expr.const.imm], [expr.const.core], [expr.call]
// XFAIL: gcc reports an additional error at the member read
struct Gauge {
    int bias;

    consteval int adjusted(int delta) const {
        return this->bias + delta;
    }
};

int runtime_receiver(Gauge gauge) {
    return gauge.adjusted(2); //! error
}

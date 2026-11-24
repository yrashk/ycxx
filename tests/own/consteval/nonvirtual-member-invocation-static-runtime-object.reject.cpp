// [expr.const.imm], [expr.call], [expr.ref]
// XFAIL: gcc accepts a non-constexpr discarded object expression
struct Gauge {
    static consteval int doubled(int value) {
        return 2 * value;
    }
};

Gauge make_gauge() {
    return {};
}

int runtime_object() {
    return make_gauge().doubled(21); //! error
}

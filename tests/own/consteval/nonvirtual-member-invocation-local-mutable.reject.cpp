// [expr.const.core], [expr.const.init], [basic.life]
// XFAIL: gcc reports an additional error at the mutable member read
struct Record {
    int value;
    mutable int cache;

    consteval int read_cache() const {
        return cache;
    }
};

int read_outside_mutable_subobject() {
    constexpr Record record{42, 7};
    return record.read_cache(); //! error
}

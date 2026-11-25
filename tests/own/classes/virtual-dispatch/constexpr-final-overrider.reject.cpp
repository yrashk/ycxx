// [expr.const.core], [class.virtual], [dcl.fct.default]
struct Base {
    virtual constexpr int value() const { return 1; }
};
struct Other : public Base {
    int value() const override { return 3; }
};
constexpr int read(const Base& base) { return base.value(); } //! error[gcc]
constexpr int test(bool flag) {
    if (!flag) return 0;
    Other other;
    return read(other);
}
static_assert(test(true) == 3); //! error

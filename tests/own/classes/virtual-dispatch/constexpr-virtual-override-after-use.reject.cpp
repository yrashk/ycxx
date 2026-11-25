// [expr.const.core], [class.virtual], [dcl.fct.default]
struct Base {
    virtual constexpr int value() const { return 1; }
};
constexpr int read(const Base& base) { return base.value(); } //! error[gcc]
struct Late : public Base {
    constexpr int value() const override;
};
constexpr int test(bool flag) {
    if (!flag) return 0;
    Late late;
    return read(late);
}
static_assert(test(true) == 2); //! error
constexpr int Late::value() const { return 2; }

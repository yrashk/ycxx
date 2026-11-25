// [expr.const.core], [class.virtual], [dcl.fct.default]
struct Base {
    virtual constexpr int value() const { return 1; }
};
struct Derived : public Base {
    constexpr int value() const override { return 2; }
};
constexpr int read(const Base& base) { return base.value(); }
constexpr int test(bool flag) {
    if (!flag) return 0;
    const Base* saved = nullptr;
    {
        Derived d;
        saved = &d;
    }
    return read(*saved);
}
static_assert(test(true) == 2); //! error

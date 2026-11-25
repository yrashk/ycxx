// [expr.const.core], [class.virtual], [dcl.fct.default]
struct Gauge {
    virtual constexpr int read(int value = 4) const { return 100 + value; }
    constexpr int indirect() const { return read(); }
};
struct Adjusted : public Gauge {
    constexpr int stamp() const { return 200; }
    constexpr int read(int value = 9) const override { return stamp() + 2 * value; }
};
struct Inherited : public Gauge {};
struct RuntimeOverride : public Gauge {
    int read(int value) const override { return 300 + value; }
};
constexpr int dispatch(const Gauge& gauge) { return gauge.read(); }
constexpr int qualified(const Gauge& gauge) { return gauge.Gauge::read(); }
constexpr bool compare() {
    Gauge root;
    Adjusted derived;
    Inherited inherited;
    RuntimeOverride runtime;
    const Gauge& base = derived;
    return dispatch(root) == 104 && dispatch(base) == 208 && dispatch(inherited) == 104 &&
           base.indirect() == 208 && derived.read() == 218 && qualified(base) == 104 &&
           qualified(runtime) == 104;
}
static_assert(compare());
struct Abstract {
    virtual int code() const = 0;
};
struct Concrete : public Abstract {
    constexpr int code() const override { return 77; }
};
constexpr int from_abstract(const Abstract& obj) { return obj.code(); }
constexpr int call_concrete() {
    Concrete concrete;
    return from_abstract(concrete);
}
static_assert(call_concrete() == 77);
int runtime_value(const Gauge& gauge) { return dispatch(gauge); }

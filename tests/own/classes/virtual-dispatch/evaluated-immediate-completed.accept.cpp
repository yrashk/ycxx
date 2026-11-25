// [expr.call], [class.virtual], [dcl.fct.default], [expr.const.core]
struct Dial {
    virtual consteval int read(int offset = 4) const { return 100 + offset; }
    consteval int read_indirect() const { return read(); }
    consteval int read_qualified() const { return Dial::read(); }
};

struct AdjustedDial : public Dial {
    consteval int read(int offset = 9) const override { return 200 + 2 * offset; }
};

consteval bool readings_match(const Dial& dial, int expected) {
    return dial.read() == expected && dial.read_indirect() == expected &&
           dial.Dial::read() == 104 && dial.read_qualified() == 104;
}

consteval bool completed_dial_readings() {
    Dial plain;
    AdjustedDial adjusted;
    const Dial& base = adjusted;
    return readings_match(plain, 104) && readings_match(base, 208) &&
           adjusted.read() == 218;
}

static_assert(completed_dial_readings());

bool read_in_ordinary_context() { return completed_dial_readings(); }

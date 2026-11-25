// [expr.const.core]
struct Dial {
    virtual consteval int read() const { return 42; }
};

void check_unknown_dial(const Dial& dial) {
    static_assert(dial.read() == 42); //! error
}

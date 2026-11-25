// #1178, [expr.const], [class.mem.general]: a constexpr friend's body is complete-class context, so
// a constant expression in the class's own member list cannot call it.
struct ios_base {
    enum fmtflags : unsigned { dec = 1, hex = 2 };
    friend constexpr fmtflags operator~(fmtflags f) { return fmtflags(~unsigned(f)); }
    friend constexpr fmtflags& operator|=(fmtflags& a, fmtflags b) { return a = fmtflags(unsigned(a) | unsigned(b)); }
    static constexpr fmtflags nothex = ~hex; //! error
    static_assert(~dec != dec); //! error
};
static_assert(~ios_base::dec != ios_base::dec);

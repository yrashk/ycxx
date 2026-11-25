// #1178, [class.friend]/6-7, [bitmask.types], [over.unary], [over.assign], [basic.lookup.argdep]/3:
// the ios_base shape. The class is an associated entity of its member enumerations.
// EXIT: 0
namespace io {
class ios_base {
public:
    virtual ~ios_base() { ++destroyed; }

    enum fmtflags : unsigned { boolalpha = 1, dec = 2, hex = 4, oct = 8, basefield = 14, all = ~0u };
    friend constexpr fmtflags operator~(fmtflags f) noexcept { return fmtflags(~unsigned(f)); }
    friend constexpr fmtflags operator&(fmtflags a, fmtflags b) noexcept { return fmtflags(unsigned(a) & unsigned(b)); }
    friend constexpr fmtflags operator|(fmtflags a, fmtflags b) noexcept { return fmtflags(unsigned(a) | unsigned(b)); }
    friend constexpr fmtflags operator^(fmtflags a, fmtflags b) noexcept { return fmtflags(unsigned(a) ^ unsigned(b)); }
    friend constexpr fmtflags& operator&=(fmtflags& a, fmtflags b) noexcept { return a = a & b; }
    friend constexpr fmtflags& operator|=(fmtflags& a, fmtflags b) noexcept { return a = a | b; }
    friend constexpr fmtflags& operator^=(fmtflags& a, fmtflags b) noexcept { return a = a ^ b; }

    enum iostate : unsigned { goodbit = 0, badbit = 1, eofbit = 2, failbit = 4 };
    friend constexpr iostate operator~(iostate f) noexcept { return iostate(~unsigned(f)); }
    friend constexpr iostate operator&(iostate a, iostate b) noexcept { return iostate(unsigned(a) & unsigned(b)); }
    friend constexpr iostate operator|(iostate a, iostate b) noexcept { return iostate(unsigned(a) | unsigned(b)); }
    friend constexpr iostate& operator&=(iostate& a, iostate b) noexcept { return a = a & b; }
    friend constexpr iostate& operator|=(iostate& a, iostate b) noexcept { return a = a | b; }

    fmtflags flags() const { return flags_; }
    fmtflags setf(fmtflags f) { fmtflags old = flags_; flags_ |= f; return old; }
    fmtflags setf(fmtflags f, fmtflags mask) {
        fmtflags old = flags_;
        flags_ &= ~mask;
        flags_ |= f & mask;
        return old;
    }
    void unsetf(fmtflags mask) { flags_ &= ~mask; }
    void toggle(fmtflags f) { flags_ ^= f; }
    iostate rdstate() const { return state_; }
    void setstate(iostate s) { state_ |= s; }
    void unset(iostate s) { state_ &= ~s; }

    static int destroyed;
    ios_base() {}

private:
    fmtflags flags_ = dec;
    iostate state_ = goodbit;
};
int ios_base::destroyed = 0;

class stream : public ios_base {
public:
    ~stream() override { destroyed += 10; }
    bool is_hex() const { return (flags() & basefield) == hex; }
};
}

using B = io::ios_base;
static_assert(__is_same(decltype(~B::dec), B::fmtflags));
static_assert(__is_same(decltype(B::badbit | B::eofbit), B::iostate));
static_assert((B::dec | B::hex | B::oct) == B::basefield);
static_assert(~~B::hex == B::hex && ~B::all == 0);
static_assert((B::basefield ^ B::dec) == (B::hex | B::oct));
static_assert(noexcept(~B::dec));
constexpr unsigned folded() {
    B::fmtflags f = B::boolalpha;
    static_assert(noexcept(f &= f) && noexcept(f |= f) && noexcept(f ^= f));
    f |= B::basefield;   // 15
    f &= ~B::dec;        // 13
    f ^= B::boolalpha;   // 12
    return f;
}
static_assert(folded() == 12);
static_assert((~B::badbit & (B::badbit | B::failbit)) == B::failbit);

int main() {
    {
        io::stream s;
        if (s.flags() != B::dec) return 1;
        if (s.setf(B::boolalpha) != B::dec) return 2;
        if (s.flags() != (B::boolalpha | B::dec)) return 3;
        s.setf(B::hex, B::basefield);
        if (!s.is_hex()) return 4;
        if (unsigned(s.flags()) != 5) return 5;
        s.unsetf(B::hex);
        if (s.flags() != B::boolalpha) return 6;
        s.toggle(io::stream::oct | io::stream::boolalpha);
        if (s.flags() != B::oct) return 7;
        s.setstate(B::badbit);
        s.setstate(B::failbit);
        if (unsigned(s.rdstate()) != 5) return 8;
        s.unset(B::badbit);
        if (s.rdstate() != B::failbit) return 9;
        B::fmtflags f = B::dec;
        if (&(f |= B::hex) != &f) return 10;
        ((f |= B::oct) &= ~B::dec) ^= B::boolalpha;
        if (unsigned(f) != 13) return 11;
    }
    return B::destroyed == 11 ? 0 : 12;
}

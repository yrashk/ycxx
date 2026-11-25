// #1178, [class.friend]/6, [namespace.memdef]/3: hidden friends declared in a class and defined
// later at namespace scope; ordinary and qualified lookup find them only from there on.
// EXIT: 0
namespace n {
class C {
    int v;
    static constexpr int bias = 5;

public:
    enum Kind : unsigned { none, some };
    constexpr explicit C(int i) : v(i) {}
    friend int operator~(const C&);
    friend bool operator!(const C&);
    friend constexpr C& operator++(C&);
    friend C& operator<<=(C&, int);
    friend C& operator^=(C& a, const C& b) noexcept;
    friend Kind& operator|=(Kind&, Kind) noexcept;
    friend constexpr int operator*(C);
};

// Declared, not yet defined: argument-dependent lookup finds them.
int early(C& c) { return ~(++c <<= 2); }
static_assert(noexcept(*static_cast<C*>(nullptr) ^= *static_cast<C*>(nullptr)));

int operator~(const C& c) { return c.v + C::bias; }
bool operator!(const C& c) { return c.v == 0; }
constexpr C& operator++(C& c) { ++c.v; return c; }
C& operator<<=(C& c, int n) { c.v <<= n; return c; }
C& operator^=(C& a, const C& b) noexcept { a.v ^= b.v; return a; }
C::Kind& operator|=(C::Kind& a, C::Kind b) noexcept { return a = C::Kind(unsigned(a) | unsigned(b)); }
constexpr int operator*(C c) { return c.v; }
}

constexpr int folded() { n::C c(1); return *++c; }
static_assert(folded() == 2);

int main() {
    n::C c(1);
    if (n::early(c) != 13) return 1;
    if (!c) return 2;
    if (n::operator~(c) != 13) return 3;
    c ^= n::C(8);
    if (*c != 0 || !!c) return 4;
    n::C::Kind k = n::C::none;
    k |= n::C::some;
    return k == n::C::some ? 0 : 5;
}

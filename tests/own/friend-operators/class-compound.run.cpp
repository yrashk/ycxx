// #1178, [over.assign], [over.binary], [class.friend]/6-7: all ten compound assignments as hidden
// friends of a class, returning a reference to the left operand, and chained.
// EXIT: 0
namespace n {
class V {
    int v;

public:
    constexpr explicit V(int i) : v(i) {}
    constexpr int get() const { return v; }
    friend constexpr V& operator+=(V& a, const V& b) { a.v += b.v; return a; }
    friend constexpr V& operator-=(V& a, const V& b) { a.v -= b.v; return a; }
    friend constexpr V& operator*=(V& a, const V& b) { a.v *= b.v; return a; }
    friend constexpr V& operator/=(V& a, const V& b) { a.v /= b.v; return a; }
    friend constexpr V& operator%=(V& a, const V& b) { a.v %= b.v; return a; }
    friend constexpr V& operator<<=(V& a, const V& b) { a.v <<= b.v; return a; }
    friend constexpr V& operator>>=(V& a, const V& b) { a.v >>= b.v; return a; }
    friend constexpr V& operator&=(V& a, const V& b) { a.v &= b.v; return a; }
    friend constexpr V& operator|=(V& a, const V& b) { a.v |= b.v; return a; }
    friend constexpr V& operator^=(V& a, const V& b) { a.v ^= b.v; return a; }
};
}

constexpr int check() {
    n::V a(10), b(3), c(2);
    static_assert(__is_same(decltype(a += b), n::V&));
    if (&(a += b) != &a || a.get() != 13) return 1;
    if (&((a += b) += c) != &a || a.get() != 18) return 2;
    if ((a -= b).get() != 15) return 3;
    if ((a *= c).get() != 30) return 4;
    if ((a /= b).get() != 10) return 5;
    if ((a %= b).get() != 1) return 6;
    if ((a <<= b).get() != 8) return 7;
    if ((a >>= c).get() != 2) return 8;
    if ((a |= b).get() != 3) return 9;
    if ((a &= c).get() != 2) return 10;
    if ((a ^= b).get() != 1) return 11;
    // Right to left without parentheses.
    n::V x(1), y(2), z(3);
    x += y += z;
    if (x.get() != 6 || y.get() != 5 || z.get() != 3) return 12;
    ((x -= y) *= z) <<= z;
    if (x.get() != 24) return 13;
    return 0;
}
static_assert(check() == 0);

int main() { return check(); }

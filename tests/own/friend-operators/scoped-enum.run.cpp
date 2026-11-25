// #1178, [over.unary], [over.assign], [over.inc], [basic.lookup.argdep]/3: every operator of the
// feature on a nested scoped enumeration, at run time and in constant expressions.
// EXIT: 0
struct Host {
    enum class E : unsigned char { zero, one, two, three, four, eight = 8 };
    friend constexpr int operator+(E e) { return int(e); }
    friend constexpr E operator~(E e) { return E(~unsigned(e) & 15u); }
    friend constexpr bool operator!(E e) { return e == E::zero; }
    friend constexpr int operator*(E e) { return int(e) * 100; }
    friend constexpr E& operator++(E& e) { return e = E(int(e) + 1); }
    friend constexpr E& operator--(E& e) { return e = E(int(e) - 1); }
    friend constexpr E& operator+=(E& e, int n) { return e = E(int(e) + n); }
    friend constexpr E& operator-=(E& e, int n) { return e = E(int(e) - n); }
    friend constexpr E& operator*=(E& e, int n) { return e = E(int(e) * n); }
    friend constexpr E& operator/=(E& e, int n) { return e = E(int(e) / n); }
    friend constexpr E& operator%=(E& e, int n) { return e = E(int(e) % n); }
    friend constexpr E& operator<<=(E& e, int n) { return e = E(int(e) << n); }
    friend constexpr E& operator>>=(E& e, int n) { return e = E(int(e) >> n); }
    friend constexpr E& operator&=(E& e, E f) { return e = E(int(e) & int(f)); }
    friend constexpr E& operator|=(E& e, E f) { return e = E(int(e) | int(f)); }
    friend constexpr E& operator^=(E& e, E f) { return e = E(int(e) ^ int(f)); }
};

using E = Host::E;
static_assert(+E::three == 3 && ~E::one == E(14) && !E::zero && !!E::one && *E::two == 200);

constexpr int check() {
    E e = E::one;
    if (++e != E::two) return 1;
    if (--e != E::one) return 2;
    if (+ ++ ++e != 3) return 3;
    if ((e += 5) != E::eight) return 4;
    if ((e -= 2) != E(6)) return 5;
    if ((e *= 2) != E(12)) return 6;
    if ((e /= 4) != E::three) return 7;
    if ((e %= 2) != E::one) return 8;
    if ((e <<= 3) != E::eight) return 9;
    if ((e >>= 1) != E::four) return 10;
    if ((e |= E::three) != E(7)) return 11;
    if ((e &= ~E::one) != E(6)) return 12;
    if ((e ^= E::four) != E::two) return 13;
    if (&(++e) != &e || e != E::three) return 14;
    return 0;
}
static_assert(check() == 0);

int main() { return check(); }

// #1178, [temp.dep.candidate], [temp.point], [basic.lookup.argdep]: dependent operands in function
// templates; the friends of classes defined after the template are found at instantiation.
// EXIT: 0
template <class T> constexpr auto plus(const T& t) { return +t; }
template <class T> constexpr auto inv(T t) { return ~t; }
template <class T> constexpr bool no(const T& t) { return !t; }
template <class T> constexpr decltype(auto) deref(T& t) { return *t; }
template <class T> constexpr auto addr(T& t) { return &t; }
template <class T> constexpr T& inc(T& t) { return ++t; }
template <class T> constexpr T& dec(T& t) { return --t; }
template <class T, class U> constexpr decltype(auto) apply(int op, T& t, const U& u) {
    switch (op) {
    case 0: return t += u;
    case 1: return t -= u;
    case 2: return t *= u;
    case 3: return t /= u;
    case 4: return t %= u;
    case 5: return t <<= u;
    case 6: return t >>= u;
    case 7: return t &= u;
    case 8: return t |= u;
    default: return t ^= u;
    }
}
template <class T> constexpr T twice(T t) { (t += t) += 1; return t; }

namespace later {
class Int {
    int v;

public:
    constexpr Int(int i) : v(i) {}
    constexpr int get() const { return v; }
    friend constexpr int operator+(const Int& x) { return x.v + 1; }
    friend constexpr Int operator~(const Int& x) { return ~x.v; }
    friend constexpr bool operator!(const Int& x) { return x.v == 0; }
    friend constexpr int& operator*(Int& x) { return x.v; }
    friend constexpr char operator&(Int&) { return 'a'; }
    friend constexpr Int& operator++(Int& x) { ++x.v; return x; }
    friend constexpr Int& operator--(Int& x) { --x.v; return x; }
    friend constexpr Int& operator+=(Int& x, const Int& y) { x.v += y.v; return x; }
    friend constexpr Int& operator-=(Int& x, const Int& y) { x.v -= y.v; return x; }
    friend constexpr Int& operator*=(Int& x, const Int& y) { x.v *= y.v; return x; }
    friend constexpr Int& operator/=(Int& x, const Int& y) { x.v /= y.v; return x; }
    friend constexpr Int& operator%=(Int& x, const Int& y) { x.v %= y.v; return x; }
    friend constexpr Int& operator<<=(Int& x, const Int& y) { x.v <<= y.v; return x; }
    friend constexpr Int& operator>>=(Int& x, const Int& y) { x.v >>= y.v; return x; }
    friend constexpr Int& operator&=(Int& x, const Int& y) { x.v &= y.v; return x; }
    friend constexpr Int& operator|=(Int& x, const Int& y) { x.v |= y.v; return x; }
    friend constexpr Int& operator^=(Int& x, const Int& y) { x.v ^= y.v; return x; }
};
struct Host {
    enum class E { a, b, c };
    friend constexpr E operator~(E e) { return e == E::a ? E::c : E::a; }
    friend constexpr E& operator++(E& e) { return e = E(int(e) + 1); }
};
}

constexpr int check() {
    later::Int x(6);
    if (plus(x) != 7) return 1;
    if (inv(x).get() != -7) return 2;
    if (no(x) || !no(later::Int(0))) return 3;
    deref(x) = 20;
    if (x.get() != 20) return 4;
    if (addr(x) != 'a') return 5;
    if (inc(inc(x)).get() != 22 || dec(x).get() != 21) return 6;
    const int want[] = {24, 18, 63, 7, 0, 168, 2, 1, 23, 22};
    for (int op = 0; op < 10; ++op) {
        later::Int t(21);
        if (apply(op, t, later::Int(3)).get() != want[op]) return 10 + op;
    }
    if (twice(x).get() != 43) return 7;
    // The same templates with arithmetic operands use the built-in operators.
    int i = 6, *p = &i;
    if (plus('a') != 'a' || inv(5) != -6 || no(1) || deref(p) != 6 || addr(i) != p) return 8;
    if (inc(i) != 7 || twice(4) != 9) return 9;
    auto e = later::Host::E::a;
    if (inv(e) != later::Host::E::c || inc(e) != later::Host::E::b) return 30;
    return 0;
}
static_assert(check() == 0);

int main() { return check(); }

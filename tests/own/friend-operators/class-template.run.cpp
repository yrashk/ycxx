// #1178, [temp.friend], [temp.inst]: hidden friend operators of class template specializations,
// with a member enumeration, and partial and explicit specializations.
// EXIT: 0
namespace n {
template <class T>
class Num {
    T v;

public:
    enum class Flag : unsigned { a = 1, b = 2 };
    constexpr Num(T t) : v(t) {}
    constexpr T get() const { return v; }
    friend constexpr Num operator+(const Num& x) { return x; }
    friend constexpr Num operator~(const Num& x) { return Num(~x.v); }
    friend constexpr bool operator!(const Num& x) { return !x.v; }
    friend constexpr T& operator*(Num& x) { return x.v; }
    friend constexpr T operator&(Num x) { return x.v; }
    friend constexpr Num& operator++(Num& x) { ++x.v; return x; }
    friend constexpr Num& operator--(Num& x) { --x.v; return x; }
    friend constexpr Num& operator+=(Num& x, const Num& y) { x.v += y.v; return x; }
    friend constexpr Num& operator<<=(Num& x, int n) { x.v <<= n; return x; }
    friend constexpr Flag operator~(Flag f) { return Flag(~unsigned(f) & 3); }
    friend constexpr Flag& operator|=(Flag& f, Flag g) { return f = Flag(unsigned(f) | unsigned(g)); }
};

template <class T>
struct Num<T*> {
    friend constexpr int operator~(const Num&) { return -7; }
};
template <>
struct Num<bool> {
    friend constexpr int operator~(Num) { return -8; }
    friend constexpr int operator%=(Num&, Num) { return 9; }
};

template <class T> struct Box { T t; };
struct Arg {
    int v;
    // Found for Box<Arg>: the classes of template type arguments are associated.
    friend constexpr int operator!(const Box<Arg>& b) { return b.t.v + 1; }
};
}

constexpr int check() {
    n::Num<int> i(5);
    n::Num<long> l(6);
    if ((+i).get() != 5) return 1;
    if ((~l).get() != ~6L) return 2;
    if (!i || !n::Num<int>(0) != true) return 3;
    *i = 7;
    if (&i != 7) return 4;
    if ((++++i).get() != 9 || (--l).get() != 5) return 5;
    if (((i += i) += n::Num<int>(2)).get() != 20) return 6;
    if ((l <<= 2).get() != 20) return 7;
    auto f = n::Num<char>::Flag::a;
    f |= ~f;
    if (unsigned(f) != 3) return 8;
    if (~n::Num<int*>() != -7) return 9;
    n::Num<bool> b;
    if (~b != -8 || (b %= b) != 9) return 10;
    if (!n::Box<n::Arg>{{4}} != 5) return 12;
    return 0;
}
static_assert(__is_same(decltype(&n::Num<short>(1)), short));

int main() { return check(); }

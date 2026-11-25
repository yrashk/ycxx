// #1178, [expr.prim.fold], [over.match.oper]: hidden compound-assignment friends as fold operators.
// EXIT: 0
namespace n {
struct Acc {
    int v;
    friend constexpr Acc& operator+=(Acc& a, int i) { a.v = a.v * 10 + i; return a; }
    friend constexpr Acc& operator|=(Acc& a, const Acc& b) { a.v |= b.v; return a; }
    friend constexpr int operator>>=(Acc a, int& out) { return out = a.v; }
};
}
template <class T, class... Us> constexpr T& left(T& t, const Us&... us) { return (t += ... += us); }
template <class... Ts> constexpr auto& right(Ts&... ts) { return (ts |= ...); }
template <class... Ts> constexpr int init(int& out, const Ts&... ts) { return (ts >>= ... >>= out); }

constexpr int check() {
    n::Acc a{0};
    if (&left(a, 1, 2, 3) != &a || a.v != 123) return 1;
    n::Acc x{1}, y{2}, z{4};
    if (right(x, y, z).v != 7 || y.v != 6 || z.v != 4) return 2;
    int out = 0;
    if (init(out, z) != 4 || out != 4) return 3;
    return 0;
}
static_assert(check() == 0);

int main() { return check(); }

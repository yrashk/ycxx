// #1178, [over.oper.general], [basic.lookup.argdep]: hidden friend operators called in function
// notation, with deduced and decltype(auto) return types.
// EXIT: 0
namespace n {
class F {
    int v = 3;

public:
    enum class K { p, q };
    friend auto operator~(const F& f) { return f.v + 0.5; }
    friend decltype(auto) operator*(F& f) { return (f.v); }
    friend auto operator!(const F& f) -> int { return f.v; }
    friend auto& operator^=(F& f, int i) { f.v ^= i; return f; }
    friend constexpr auto operator--(K& k) { return k = K::p; }
};
}

int main() {
    n::F f;
    if (operator~(f) != 3.5 || ~f != 3.5) return 1;
    operator*(f) = 4;
    if (*f != 4 || operator!(f) != 4 || !f != 4) return 2;
    if (*operator^=(f, 1) != 5) return 3;
    operator^=(f ^= 1, 8) ^= 2;
    if (*f != 14) return 4;
    auto k = n::F::K::q;
    static_assert(__is_same(decltype(operator--(k)), n::F::K));
    operator--(k);
    return k == n::F::K::p ? 0 : 5;
}

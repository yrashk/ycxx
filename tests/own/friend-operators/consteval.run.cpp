// #1178, [dcl.constexpr], [expr.const], [class.friend]/6: consteval hidden friends.
// EXIT: 0
namespace n {
struct C {
    int v;
    enum class E : unsigned { a = 1, b = 2 };
    friend consteval C operator~(const C& c) { return {-c.v}; }
    friend consteval bool operator!(C c) { return c.v == 0; }
    friend consteval int operator*(const C& c) { return c.v; }
    friend consteval C& operator++(C& c) { ++c.v; return c; }
    friend consteval C& operator+=(C& c, int i) { c.v += i; return c; }
    friend consteval E operator~(E e) { return E(~unsigned(e) & 3); }
};
template <class T> struct D {
    T t;
    friend consteval T operator+(const D& d) { return d.t + 1; }
};
}
consteval int sum() {
    n::C c{1};
    (++c += 5) += 6;
    return *c;
}
static_assert(*~n::C{4} == -4 && !n::C{0} && sum() == 13);
constexpr n::C neg = ~n::C{8};
static_assert(~n::C::E::a == n::C::E::b);

int main() {
    if (*n::C{3} != 3) return 1;
    if (sum() != 13) return 2;
    if (neg.v != -8) return 3;
    int array[+n::D<int>{2}];
    return sizeof(array) == 3 * sizeof(int) ? 0 : 4;
}

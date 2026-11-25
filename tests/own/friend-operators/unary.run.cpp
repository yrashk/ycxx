// #1178, [over.unary], [class.friend]/6-7: unary + ~ ! * & as hidden friends of a class, nested
// in one another, in conditions, on subobjects and array elements.
// EXIT: 0
namespace n {
class P {
    int v;

public:
    constexpr P(int i) : v(i) {}
    friend constexpr P operator+(const P& p) { return P(p.v + 100); }
    friend constexpr P operator~(const P& p) { return P(~p.v); }
    friend constexpr bool operator!(const P& p) { return p.v == 0; }
    friend constexpr int& operator*(P& p) { return p.v; }
    friend constexpr const int& operator*(const P& p) { return p.v; }
    friend constexpr long operator&(const P& p) { return p.v * 2L; }
};
}

struct Holder {
    n::P a[2];
    n::P* ptr;
};

static_assert(*+n::P(1) == 101 && *~n::P(0) == -1 && !n::P(0) && !!n::P(1) && &n::P(4) == 8);
static_assert(__is_same(decltype(&n::P(1)), long));
static_assert(__is_same(decltype(!n::P(1)), bool));

int main() {
    n::P p(5);
    const n::P c(6);
    *p = 7;
    if (*p != 7) return 1;
    if (*c != 6) return 2;
    static_assert(__is_same(decltype(*p), int&) && __is_same(decltype(*c), const int&));
    ++*p;
    if (*+p != 108) return 3;
    if (*~~p != 8) return 4;
    if (!p) return 5;
    if (&p != 16) return 6;
    if (&+c != 212) return 7;
    Holder h{{n::P(1), n::P(2)}, nullptr};
    h.ptr = h.a;
    if (*~h.a[1] != -3) return 8;
    // A pointer operand is not of class type: the built-in operators apply.
    if (*(*h.ptr) != 1 || !h.ptr) return 9;
    if (**h.ptr != 1 || *(h.ptr[1]) != 2) return 10;
    *h.a[0] = 0;
    int n = !h.a[0] ? 1 : 2;
    while (!h.a[0]) *h.a[0] = 3;
    return n == 1 && *h.a[0] == 3 ? 0 : 11;
}

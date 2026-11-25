// #1178, [stmt.ranged], [over.match.oper]: a range-based for statement whose iterator's unary *
// and prefix ++ are hidden friends.
// EXIT: 0
namespace n {
class It {
    int i;

public:
    constexpr It(int n) : i(n) {}
    friend constexpr int operator*(const It& it) { return it.i * it.i; }
    friend constexpr It& operator++(It& it) { ++it.i; return it; }
    friend constexpr bool operator==(const It& a, const It& b) { return a.i == b.i; }
};
struct Range {
    int n;
    constexpr It begin() const { return It(0); }
    constexpr It end() const { return It(n); }
};
template <class T> struct Ptr {
    T* p;
    friend constexpr T& operator*(Ptr it) { return *it.p; }
    friend constexpr Ptr& operator++(Ptr& it) { ++it.p; return it; }
    friend constexpr bool operator!=(Ptr a, Ptr b) { return a.p != b.p; }
};
template <class T> struct Span {
    T *b, *e;
    constexpr Ptr<T> begin() { return {b}; }
    constexpr Ptr<T> end() { return {e}; }
};
}
template <class R> constexpr int total(R r) {
    int sum = 0;
    for (auto&& x : r) sum += x;
    return sum;
}

constexpr int check() {
    int sum = 0;
    for (int sq : n::Range{4}) sum += sq;
    if (sum != 14) return 1;
    int a[] = {1, 2, 3};
    for (int& x : n::Span<int>{a, a + 3}) x *= 2;
    if (a[0] != 2 || a[2] != 6) return 2;
    if (total(n::Range{3}) != 5) return 3;
    return total(n::Span<int>{a, a + 3}) == 12 ? 0 : 4;
}
static_assert(check() == 0);

int main() { return check(); }

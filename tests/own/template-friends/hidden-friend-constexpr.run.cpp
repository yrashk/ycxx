// [dcl.constexpr], [expr.const], [temp.friend]: a constexpr or consteval friend a class
// template defines is usable in constant expressions for each specialization.
namespace n {
template <class T>
struct E {
    T v;
    friend constexpr T val(const E& e) { return e.v; }
    friend consteval T twice(const E& e) { return e.v * 2; }
    constexpr T doubled() const { return val(*this) * 2; }
};
}

static_assert(val(n::E<int>{4}) == 4);
static_assert(n::E<int>{4}.doubled() == 8);
static_assert(twice(n::E<long>{5}) == 10);
int bound[val(n::E<int>{3})];

int main() {
    // A reference to a block-scope constexpr object is #1159.
    constexpr int t = twice(n::E<int>{6});
    n::E<int> e{6};
    return sizeof(bound) == 3 * sizeof(int) && t == 12 && val(e) == 6 ? 0 : 1;
}

// [class.compare.default], [class.eq], [over.match.oper]: a defaulted equality operator declared
// as a friend, with parameters of the class type or of `const C&`.
struct O {
    int v;
    friend constexpr bool operator==(O, O) noexcept = default;
};

struct R {
    int a;
    long b;
    friend bool operator==(const R&, const R&) = default;
};

// A private member, compared through the friend; the subobjects in declaration order.
class Ordering {
    signed char v_;

public:
    constexpr explicit Ordering(signed char v) : v_(v) {}
    friend constexpr bool operator==(Ordering, Ordering) noexcept = default;
};

// A base, a class member with its own `==`, and an array member.
struct Base {
    int x;
    friend constexpr bool operator==(const Base&, const Base&) = default;
};
struct Inner {
    int i;
    constexpr bool operator==(const Inner& o) const { return i == o.i; }
};
struct Derived : Base {
    Inner in;
    int arr[3];
    friend constexpr bool operator==(const Derived&, const Derived&) = default;
};

// A class with no members compares equal.
struct Empty {
    friend constexpr bool operator==(Empty, Empty) = default;
};

static_assert(O{1} == O{1});
static_assert(!(O{1} == O{2}));
static_assert(Ordering(1) == Ordering(1));
static_assert(!(Ordering(-1) == Ordering(1)));
static_assert(Empty{} == Empty{});
static_assert(noexcept(O{0} == O{0}));

// [except.spec]: without a noexcept-specifier, potentially-throwing exactly when a comparison
// of the implicit definition is.
struct Throwing {
    int v;
    bool operator==(const Throwing&) const { return true; }
};
struct HasThrowing {
    Throwing t;
    friend bool operator==(const HasThrowing&, const HasThrowing&) = default;
};
static_assert(noexcept(R{1, 2} == R{1, 2}));
static_assert(!noexcept(HasThrowing{} == HasThrowing{}));
static_assert(__is_same(decltype(O{0} == O{0}), bool));

int main() {
    O a{0}, b{0}, c{3};
    R r{1, 2}, s{1, 2}, t{1, 3};
    // At run time: constant evaluation of a class with a class or array subobject is #1140.
    Derived d{{1}, {2}, {3, 4, 5}}, e{{1}, {2}, {3, 4, 5}};
    Derived base{{0}, {2}, {3, 4, 5}}, inner{{1}, {9}, {3, 4, 5}}, last{{1}, {2}, {3, 4, 6}};
    return a == b && !(a == c) && r == s && !(r == t) && d == e && !(d == base) && !(d == inner) &&
           !(d == last) && d != last ? 0 : 1;
}

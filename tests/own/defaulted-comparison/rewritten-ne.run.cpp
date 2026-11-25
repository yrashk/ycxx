// [over.match.oper]/3.4: `x != y` uses `operator==` when no `operator!=`
// is declared, as `!(x == y)`; `x == y` also considers `operator==` with the operands reversed.
struct O {
    int v;
    friend constexpr bool operator==(O, O) noexcept = default;
};

struct Zero {
    constexpr Zero(int) {}
};
struct Ord {
    signed char v;
    friend constexpr bool operator==(Ord o, Zero) noexcept { return o.v == 0; }
};

struct M {
    int v;
    constexpr bool operator==(int i) const { return v == i; }
};

static_assert(O{1} != O{2});
static_assert(!(O{1} != O{1}));
static_assert(Ord{0} == 0);
static_assert(0 == Ord{0});  // reversed
static_assert(Ord{1} != 0);
static_assert(0 != Ord{1});  // reversed and rewritten
static_assert(!(0 != Ord{0}));
static_assert(M{4} == 4 && 4 == M{4} && M{4} != 5 && 5 != M{4});
static_assert(noexcept(O{1} != O{2}));
static_assert(__is_same(decltype(O{1} != O{2}), bool));

// A declared `operator!=` is used, not the rewritten form.
struct Both {
    int v;
    friend constexpr bool operator==(Both, Both) { return true; }
    friend constexpr bool operator!=(Both, Both) { return true; }
};
static_assert(Both{1} == Both{2} && Both{1} != Both{2});

int main() {
    O a{0}, b{0}, c{3};
    Ord z{0};
    return !(a != b) && a != c && !(z != 0) && !(0 != z) ? 0 : 1;
}

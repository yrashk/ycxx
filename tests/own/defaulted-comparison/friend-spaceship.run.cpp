// [class.friend], [over.binary], [basic.lookup.argdep]: a three-way comparison operator function
// defined as a hidden friend, as <compare>'s comparison categories declare theirs; called by name
// (the `<=>` expression itself is #295).
struct Zero {
    constexpr Zero(int) {}
};
struct Ord {
    signed char v;
    friend constexpr Ord operator<=>(Ord o, Zero) noexcept { return o; }
    friend constexpr Ord operator<=>(Zero, Ord o) noexcept { return Ord{static_cast<signed char>(-o.v)}; }
};

static_assert(operator<=>(Ord{1}, 0).v == 1);
static_assert(operator<=>(0, Ord{1}).v == -1);

int main() {
    Ord o{-1};
    return operator<=>(o, 0).v == -1 && operator<=>(0, o).v == 1 ? 0 : 1;
}

// [expr.type.conv]/1, [dcl.type.auto.deduct], [dcl.init.general]/16.6.1 (#286): a class operand
// is copied into a new prvalue of its decayed type. A prvalue operand initializes the result
// directly, with no copy.
int copies = 0, moves = 0;

struct Counted {
    int value;
    Counted(int v) : value(v) {}
    Counted(const Counted& other) : value(other.value) { ++copies; }
    Counted(Counted&& other) : value(other.value) { ++moves; other.value = -1; }
};

Counted make(int v) { return Counted(v); }

static_assert(__is_same(decltype(auto(make(1))), Counted));
const Counted constant(2);
static_assert(__is_same(decltype(auto(constant)), Counted));
static_assert(__is_same(decltype(auto{constant}), Counted));

int main() {
    Counted original(7);
    Counted parens = auto(original);
    if (copies != 1 || moves != 0 || parens.value != 7)
        return 1;
    parens.value = 8;  // a copy, not the original
    if (original.value != 7)
        return 2;
    Counted braces = auto{original};
    if (copies != 2 || moves != 0 || braces.value != 7)
        return 3;
    // A prvalue is not moved or copied.
    Counted direct = auto(make(9));
    Counted direct_braced = auto{make(10)};
    if (copies != 2 || moves != 0 || direct.value != 9 || direct_braced.value != 10)
        return 4;
    // A const lvalue copies into a modifiable prvalue.
    if (auto(constant).value != 2 || copies != 3)
        return 5;
    // An xvalue moves.
    Counted moved = auto(static_cast<Counted&&>(original));
    if (moves != 1 || moved.value != 7 || original.value != -1)
        return 6;
    return 0;
}

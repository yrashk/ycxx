// #1178, [over.match.oper], [over.ics.rank]: hidden friends taking class operands by value and by
// reference; the operand's value category and cv-qualification choose among them.
// EXIT: 0
struct Ref {
    friend int operator+(Ref&) { return 1; }
    friend int operator+(const Ref&) { return 2; }
    friend int operator+(Ref&&) { return 3; }
    friend int operator~(const Ref&&) { return 4; }
    friend int operator~(const Ref&) { return 5; }
    friend int operator++(Ref&) { return 6; }
    friend int operator++(Ref&&) { return 7; }
    friend int operator*=(Ref&, Ref&) { return 8; }
    friend int operator*=(Ref&, const Ref&) { return 9; }
    friend int operator*=(Ref&, Ref&&) { return 10; }
    friend int operator*=(const Ref&, Ref&&) { return 11; }
    friend int operator*=(Ref&&, const Ref&) { return 12; }
};

struct Counted {
    int* copies;
    Counted(int* c) : copies(c) {}
    Counted(const Counted& o) : copies(o.copies) { ++*copies; }
    friend int operator!(Counted c) { return *c.copies; }
    friend Counted operator+=(Counted a, Counted b) { return Counted(a.copies); }
};

int main() {
    Ref r;
    const Ref c{};
    if (+r != 1) return 1;
    if (+c != 2) return 2;
    if (+Ref{} != 3) return 3;
    if (+static_cast<Ref&&>(r) != 3) return 4;
    if (~r != 5 || ~c != 5) return 5;
    if (~Ref{} != 4 || ~static_cast<const Ref&&>(c) != 4) return 6;
    if (++r != 6) return 7;
    if (++Ref{} != 7) return 8;
    if ((r *= r) != 8) return 9;
    if ((r *= c) != 9) return 10;
    if ((r *= Ref{}) != 10) return 11;
    if ((c *= Ref{}) != 11) return 12;
    if ((Ref{} *= c) != 12) return 13;
    Ref&& named = Ref{};
    if (+named != 1) return 14;

    int copies = 0;
    Counted k(&copies);
    if (!k != 1) return 15;
    if (!Counted(&copies) != 1) return 16;  // elided
    Counted sum = k += k;
    if (copies != 3) return 17;
    return 0;
}

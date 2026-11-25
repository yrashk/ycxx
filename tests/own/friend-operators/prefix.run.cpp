// #1178, [over.inc], [over.match.oper]: prefix ++ and -- as hidden friends of a class and of a
// class with an unscoped member enumeration, with reference, value and void results.
// EXIT: 0
class Counter {
    int v = 0;

public:
    constexpr int get() const { return v; }
    friend constexpr Counter& operator++(Counter& c) { ++c.v; return c; }
    friend constexpr Counter& operator--(Counter& c) { --c.v; return c; }
};

struct Day {
    enum Name { mon, tue, wed, count };
    friend constexpr Name& operator++(Name& n) { return n = Name((n + 1) % count); }
    friend constexpr Name operator--(Name& n) { return n = n == mon ? wed : Name(n - 1); }
};

struct Sink {
    int hits = 0;
    friend void operator++(Sink& s) { s.hits += 2; }
    friend int operator--(const Sink& s) { return s.hits; }
};

constexpr int check() {
    Counter c;
    if ((++c).get() != 1) return 1;
    if (&++c != &c || c.get() != 2) return 2;
    ++++++c;
    if (c.get() != 5) return 3;
    --(++c);
    ----c;
    if (c.get() != 3) return 4;
    for (; c.get() < 10; ++c) {}
    if (c.get() != 10) return 5;

    Day::Name d = Day::tue;
    if (++d != Day::wed || ++d != Day::mon) return 6;
    if (--d != Day::wed) return 7;
    ++++d;
    if (d != Day::tue) return 8;
    static_assert(__is_same(decltype(++d), Day::Name&) && __is_same(decltype(--d), Day::Name));
    return 0;
}
int main() {
    if (int r = check()) return r;
    Sink s;
    static_assert(__is_same(decltype(++s), void));
    ++s;
    ++s;
    if (--s != 4) return 9;
    if (--static_cast<const Sink&>(s) != 4) return 10;
    Sink arr[2];
    Sink* p = arr;
    ++p;  // built-in: the operand is a pointer
    if (p != arr + 1) return 11;
    ++*p;
    return arr[1].hits == 2 ? 0 : 12;
}

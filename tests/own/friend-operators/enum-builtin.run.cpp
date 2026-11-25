// #1178, [over.match.oper]/3, [over.built]: which built-in or friend operator an unscoped member
// enumeration's operand selects.
// EXIT: 0
struct C {
    enum E : unsigned { a = 1, b = 2 };
    enum F { x = 1, y = 2 };
    int v;
    constexpr C(E e) : v(e) {}
    constexpr C(F f) : v(f) {}
    friend constexpr char operator~(E e) { return 'e'; }  // exact match beats the promotion
    // With no operand of class type, a candidate needs a parameter of the operand's enumeration
    // type: these are not candidates for F, and the built-in operators apply.
    friend constexpr char operator~(const C&) { return 'c'; }
    friend constexpr char operator+(C) { return 'c'; }
    friend constexpr char operator+(const E&) { return 'r'; }
    friend constexpr char operator!(C) { return 'c'; }
    friend constexpr char operator+=(int&, C) { return 'c'; }
    friend constexpr char operator-=(int&, const E&) { return 'r'; }
    friend constexpr char operator|=(E&, unsigned) { return 'u'; }
    friend constexpr char operator&(E, E) { return 'b'; }
    friend constexpr char operator*(E, E) { return 'b'; }
};

constexpr int check() {
    static_assert(__is_same(decltype(~C::a), char) && ~C::a == 'e');
    static_assert(!__is_same(decltype(~C::x), char) && ~C::x != 'c');
    static_assert(~C(C::a) == 'c');
    static_assert(+C::a == 'r' && +C::b == 'r');
    static_assert(!__is_same(decltype(+C::x), char) && +C::y == 2);
    static_assert(__is_same(decltype(!C::x), bool) && __is_same(decltype(!C::a), bool));
    int i = 5;
    if ((i += C::y) != 7 || i != 7) return 1;   // built-in
    if ((i += C(C::y)) != 'c' || i != 7) return 2;
    if ((i -= C::b) != 'r' || i != 7) return 3;
    if ((i -= C::y) != 5) return 4;             // built-in
    C::E e = C::a;
    if ((e |= 4u) != 'u' || (e |= C::b) != 'u') return 5;
    // Binary friends do not affect the unary operators.
    C::E* p = &e;
    if (*p != C::a) return 6;
    C::F f = C::x;
    return *&f == C::x ? 0 : 7;
}
static_assert(check() == 0);

int main() { return check(); }

// #1178, [over.match.oper]/3: with no operand of class type, a candidate needs a parameter of
// the operand's enumeration type (or a reference to it); a converting class is not enough.
struct C {
    enum class E { e };
    C(E) {}
    friend int operator~(const C&) { return 1; }
    friend bool operator!(C) { return true; }
    friend int operator++(C&&) { return 2; }
    friend int operator+=(C, int) { return 3; }
    friend int operator|=(const C&, const C&) { return 4; }
};

void f(C::E e, C c) {
    ~e; //! error
    !e; //! error
    ++e; //! error
    e += 1; //! error
    e |= e; //! error
    (void)~c;
    c |= e;
    e |= c;
}

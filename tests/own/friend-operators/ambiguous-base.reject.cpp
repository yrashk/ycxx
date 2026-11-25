// #1178, [class.member.lookup], [conv.ptr]/3, [over.best.ics]: the conversion of a derived operand
// to an ambiguous base of a hidden friend's parameter is ill-formed.
struct A {
    friend int operator~(const A&) { return 1; }
    friend A& operator++(A& a) { return a; }
    friend A& operator|=(A& a, int) { return a; }
};
struct L : A {};
struct R : A {};
struct D : L, R {};

void f(D& d) {
    ~d; //! error
    ++d; //! error
    d |= 1; //! error
    ++static_cast<L&>(d);
}

// #1178, [over.match.oper], [over.ics.ref]: a const operand or a temporary does not
// bind to a hidden friend's non-const lvalue reference parameter.
struct A {
    enum E : unsigned { x, y };
    friend A& operator++(A& a) { return a; }
    friend A& operator+=(A& a, const A&) { return a; }
    friend A& operator<<=(A& a, A&) { return a; }
    friend int operator-=(A&& a, int) { return 0; }
    friend A& operator|=(A& a, A&&) { return a; }
    friend E& operator++(E& e) { return e; }
    friend int operator*(A&) { return 1; }
    friend int operator~(A&&) { return 2; }
};

void f(A& a, const A& c, const A::E e) {
    ++c; //! error
    c += a; //! error
    A{} += a; //! error
    a <<= c; //! error
    a -= 1; //! error
    a |= a; //! error
    ++e; //! error
    ++A::y; //! error
    *c; //! error
    *A{}; //! error
    ~a; //! error
    ++A{}; //! error
    int i = 0;
    i += a; //! error
    a += 1; //! error
}

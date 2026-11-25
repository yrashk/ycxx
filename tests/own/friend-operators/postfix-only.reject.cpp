// #1178, [over.inc], [over.match.oper]: a friend with the postfix signature is not a candidate for
// the prefix operator, nor a binary friend for a unary operator.
struct A {
    enum class E { e };
    friend A operator++(A& a, int) { return a; }
    friend int operator+(const A&, const A&) { return 1; }
    friend int operator*(const A&, const A&) { return 2; }
    friend E operator--(E& e, int) { return e; }
    friend int operator&(E, E) { return 3; }
};

void f(A& a, A::E& e) {
    ++a; //! error
    +a; //! error
    *a; //! error
    --e; //! error
    (void)&e;
}

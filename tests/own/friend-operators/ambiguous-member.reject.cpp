// #1178, [over.match.best], [over.match.oper]: a member operator and a hidden friend with equal
// conversions are ambiguous.
struct A {
    int operator~() const { return 1; }
    friend int operator~(const A&) { return 2; }
    bool operator!() { return true; }
    friend bool operator!(A&) { return false; }
    A& operator++() { return *this; }
    friend A& operator++(A& a) { return a; }
    A& operator+=(int) { return *this; }
    friend A& operator+=(A& a, int) { return a; }
    A& operator*=(const A&) { return *this; }
    friend A& operator*=(const A&, A& b) { return b; }
    int operator*() && { return 1; }
    friend int operator*(A&&) { return 2; }
};

void f(A& a) {
    ~a; //! error
    !a; //! error
    ++a; //! error
    a += 1; //! error
    a *= a; //! error
    *A{}; //! error
}

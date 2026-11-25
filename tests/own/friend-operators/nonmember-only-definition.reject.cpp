// #1178, [over.assign], [over.call], [over.sub], [over.ref]: nor can a friend definition.
struct A {
    friend A& operator=(A& a, int) { return a; } //! error
    friend int operator()(A&, int) { return 1; } //! error
    friend int operator[](A&, int) { return 1; } //! error
    friend A* operator->(A& a) { return &a; } //! error
};

// #1178, [over.oper.general]: an operator function cannot have default arguments.
struct A {
    friend int operator~(const A& a = A()) { return 1; } //! error
    friend A& operator+=(A& a, int i = 1) { return a; } //! error
    friend A& operator++(A& a, int = 0) { return a; } //! error
};

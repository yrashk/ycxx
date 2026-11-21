struct C {
    int member;
    C() : C(1), member(2) {} //! error
    C(int value) : member(value) {}
};

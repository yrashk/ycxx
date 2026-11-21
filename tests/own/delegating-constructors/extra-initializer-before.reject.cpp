struct C {
    int member;
    C() : member(2), C(1) {} //! error
    C(int value) : member(value) {}
};

struct C {
    C() : C(1, 2) {} //! error
    C(int) {}
};

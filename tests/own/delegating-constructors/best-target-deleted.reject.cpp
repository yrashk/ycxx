struct C {
    C() : C(1) {} //! error
    C(int) = delete;
    C(long) {}
};

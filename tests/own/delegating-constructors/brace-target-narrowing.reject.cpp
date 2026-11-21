struct C {
    C(double value) : C{value, 0} {} //! error
    C(int, int) {}
};

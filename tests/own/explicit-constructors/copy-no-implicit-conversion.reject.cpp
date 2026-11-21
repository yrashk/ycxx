struct C {
    explicit C(int);
};

C::C(int) {}

C copy = 1; //! error

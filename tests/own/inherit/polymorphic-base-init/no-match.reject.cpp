struct Base {
    explicit Base(int) {}
    virtual int read() const { return 3; }
};

struct Derived : Base {
    Derived() : Base(7, 9) {} //! error
};

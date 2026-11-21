struct Base {
    explicit Base(int) {}
    virtual int read() const { return 3; }
};

struct Middle : Base {
    Middle() : Base(7) {}
};

struct Derived : Middle {
    Derived() : Base(9) {} //! error
};

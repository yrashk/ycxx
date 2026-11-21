struct Base {
    explicit Base(long) {}
    explicit Base(unsigned long) {}
    virtual int read() const { return 3; }
};

struct Derived : Base {
    Derived() : Base(7) {} //! error
};

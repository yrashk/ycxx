struct Base {
    Base(int) = delete;
    explicit Base(long) {}
    virtual int read() const { return 3; }
};

struct Derived : Base {
    Derived() : Base(7) {} //! error
};

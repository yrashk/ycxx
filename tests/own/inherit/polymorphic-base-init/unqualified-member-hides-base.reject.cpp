namespace types {
struct Base {
    Base() = delete;
    explicit Base(int) {}
    virtual int read() const { return 3; }
};
}

struct Derived : types::Base {
    int Base;

    Derived() : Base(9) {} //! error
};

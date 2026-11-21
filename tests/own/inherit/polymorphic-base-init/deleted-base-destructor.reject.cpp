struct Base {
    explicit Base(int) {}
    ~Base() = delete;
    virtual int read() const { return 3; }
};

struct Derived : Base {
    Derived() : Base(7) {} //! error
};

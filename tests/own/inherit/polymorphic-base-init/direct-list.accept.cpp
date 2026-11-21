struct Base {
    explicit Base(int value) : value(value) {}
    virtual int read() const { return value; }

    int value;
};

struct Derived : Base {
    Derived() : Base{7} {}
};

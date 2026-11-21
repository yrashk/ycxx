namespace types {
struct Base {
    virtual int marker() const { return 7; }
};
}

struct Derived : types::Base {
    int Base;

    Derived() : Base(9) {}
};

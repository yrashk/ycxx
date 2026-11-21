constexpr int callback() { return 4; }

struct Base {
    explicit Base(int (*read)()) : value(read()) {}
    virtual int get() const { return value; }

    int value;
};

struct Derived : Base {
    Derived() : Base(&callback) {}
};

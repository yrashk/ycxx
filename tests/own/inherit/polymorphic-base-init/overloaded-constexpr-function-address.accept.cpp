constexpr int callback() { return 4; }
int callback(int);

struct Base {
    explicit Base(int (*read)()) {}
    virtual int marker() const { return 7; }
};

struct Derived : Base {
    Derived() : Base(&callback) {}
};

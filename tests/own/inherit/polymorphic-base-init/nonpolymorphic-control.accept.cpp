struct Base {
    Base() = delete;
    explicit Base(int value, int extra = 3) : total(value + extra) {}

    int total;
};

struct Derived : Base {
    Derived(bool enabled) : Base(enabled) {}
};

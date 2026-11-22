void observe(int value);

class Base {
protected:
    Base() = delete;
    explicit Base(int value, int extra = 3) {
        observe(value);
        observe(extra);
    }
    Base(double) = delete;
    ~Base() {}

public:
    virtual int marker() const { return 7; }
};

struct Derived : Base {
    Derived(bool enabled, int extra) : Base(enabled) {
        observe(extra + 1);
    }
};

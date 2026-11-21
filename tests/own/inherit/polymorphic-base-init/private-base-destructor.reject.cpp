class Base {
    ~Base() {}

public:
    explicit Base(int) {}
    virtual int read() const { return 3; }
};

struct Derived : Base { Derived() : Base(7) {} }; //! error

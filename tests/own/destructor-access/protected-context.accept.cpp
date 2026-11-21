// [class.protected]
struct Base {
    static void own();
protected:
    ~Base();
};

struct Derived : Base { static void check(); };

void Base::own() {
    Base object;
    Base{};
}

void Derived::check() {
    Base* pointer;
    Derived object;
    (void)pointer;
}

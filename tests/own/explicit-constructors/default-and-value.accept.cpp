struct A {
    explicit A() {}
};

struct B {
    explicit B(int = 7) {}
};

A a_default;
A a_list{};
A a_value = A();
B b_default;
B b_list{};
B b_value = B();

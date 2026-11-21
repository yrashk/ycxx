// [basic.scope.scope]
struct StaticFirst {
    static void f();
    void f() const &;  //! error
};

struct StaticLast {
    void f() const &&;
    static void f();  //! error
};

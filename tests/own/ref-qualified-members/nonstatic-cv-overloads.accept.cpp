// [basic.scope.scope]
struct Member {
    void f() &;
    void f() const &;
};

struct DifferentParameters {
    static void f();
    void f(int) const &;
};

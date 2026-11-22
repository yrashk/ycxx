// XFAIL: clang Clang 23 rejects the nondefining static member declaration of abstract type.
// [class.static.data] permits an incomplete type in a non-inline static declaration.
struct A {
    virtual void f() = 0;
    static A value;
};

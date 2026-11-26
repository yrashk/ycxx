template<class T>
struct A {
    virtual void f() = 0;
    static A<T> object;
};

static_assert(sizeof(A<int>) > 0);

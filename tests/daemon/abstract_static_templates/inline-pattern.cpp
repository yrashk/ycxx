template<class T> struct A {
    virtual void f() = 0;
    inline static A object;
};
A<int>* declaration_only;

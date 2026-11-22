// [basic.def]/5 requires a valid type at the inline static definition.
struct A {
    virtual void f() = 0;
    inline static A value;  //! error
};

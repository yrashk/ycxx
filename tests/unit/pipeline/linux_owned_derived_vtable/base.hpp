#ifndef YCXX_SPEC_OWNED_DERIVED_TABLE_HPP
#define YCXX_SPEC_OWNED_DERIVED_TABLE_HPP

struct B {
    virtual int f() = 0;
};

struct D : B {
    int field;
    int f() override;
};

int dispatch(B& object);
int make_and_dispatch(int value);
int check_rtti(B& base, D& complete);

#endif

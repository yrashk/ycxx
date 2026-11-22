#pragma once

struct B {
    virtual int f();
};

struct D : public B {
    int f() override;
};

extern "C" B* read_operand(B*) noexcept;
extern "C" D* downcast(B*);
extern "C" const volatile D* downcast_cv(volatile B*);
extern "C" D* downcast_constant_null();

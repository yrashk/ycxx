#ifndef YCXX_SPEC_DYNAMIC_CAST_VOID_HPP
#define YCXX_SPEC_DYNAMIC_CAST_VOID_HPP

struct B {
    virtual int identify();
};

void* top(B* source);
const void* top_const(const B* source);
volatile void* top_volatile(volatile B* source);
const volatile void* top_cv(const volatile B* source);

B* take_operand();
void* top_once();

#endif

#include "base.hpp"

void* top(B* source) { return dynamic_cast<void*>(source); }
const void* top_const(const B* source) { return dynamic_cast<const void*>(source); }
volatile void* top_volatile(volatile B* source) { return dynamic_cast<volatile void*>(source); }
const volatile void* top_cv(const volatile B* source) { return dynamic_cast<const volatile void*>(source); }

void* top_once() { return dynamic_cast<void*>(take_operand()); }

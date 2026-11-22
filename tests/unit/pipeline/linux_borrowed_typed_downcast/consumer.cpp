#include "classes.hpp"

// [expr.dynamic.cast]: typed null remains null in a constant expression.
constexpr B* null_base = nullptr;
static_assert(dynamic_cast<D*>(null_base) == nullptr);
static_assert(__is_same(decltype(dynamic_cast<const volatile D*>(static_cast<volatile B*>(nullptr))),
                        const volatile D*));

extern "C" D* downcast(B* input) {
    return dynamic_cast<D*>(read_operand(input));
}

extern "C" const volatile D* downcast_cv(volatile B* input) {
    return dynamic_cast<const volatile D*>(input);
}

extern "C" D* downcast_constant_null() {
    return dynamic_cast<D*>(null_base);
}

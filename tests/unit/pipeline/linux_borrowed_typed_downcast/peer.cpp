#include "classes.hpp"

#include <cstddef>
#include <typeinfo>

int B::f() { return 1; }
int D::f() { return 2; }

namespace {

struct L : B {};
struct Whole : L, D {};

int operand_calls;
int runtime_calls;
bool valid_runtime_call;
const volatile void* expected_source;

template<class To, class From>
bool check(To* (*cast)(From*), From* from, To* to, int expected_operand_calls) {
    expected_source = from;
    runtime_calls = 0;
    operand_calls = 0;
    valid_runtime_call = true;
    return cast(from) == to && valid_runtime_call &&
           runtime_calls == (from ? 1 : 0) && operand_calls == expected_operand_calls;
}

}  // namespace

extern "C" B* read_operand(B* input) noexcept {
    ++operand_calls;
    return input;
}

// Itanium C++ ABI §2.9.7. The descriptor addresses are compared directly, and
// the system linker forwards the call to the hosted runtime through --wrap.
extern "C" void* __real___dynamic_cast(const void*, const std::type_info*,
                                       const std::type_info*, std::ptrdiff_t);

extern "C" void* __wrap___dynamic_cast(const void* subobject, const std::type_info* source,
                                       const std::type_info* target, std::ptrdiff_t hint) {
    ++runtime_calls;
    if (!subobject || subobject != expected_source || source != &typeid(B) ||
        target != &typeid(D) || hint != 0) {
        valid_runtime_call = false;
        return nullptr;
    }
    return __real___dynamic_cast(subobject, source, target, hint);
}

int main() {
    B base;
    D derived;
    Whole whole;
    B* left = static_cast<L*>(&whole);
    D* right = &whole;
    B* null_input = nullptr;
    D* null_result = nullptr;

    // Both base paths are public. The input is the B in L and the result is the D subobject.
    if (static_cast<void*>(left) == static_cast<void*>(right)) return 1;
    if (!check(downcast, static_cast<B*>(&derived), &derived, 1)) return 2;
    if (!check(downcast, &base, null_result, 1)) return 3;
    if (!check(downcast, null_input, null_result, 1)) return 4;
    if (!check(downcast, left, right, 1)) return 5;
    if (!check<const volatile D, volatile B>(downcast_cv, left, right, 0)) return 6;
    if (!check<const volatile D, volatile B>(downcast_cv, nullptr, nullptr, 0)) return 7;
    if (downcast_constant_null() != nullptr || runtime_calls != 0 || operand_calls != 0) return 8;
}

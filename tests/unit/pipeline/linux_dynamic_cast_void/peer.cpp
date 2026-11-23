#include "base.hpp"

int B::identify() { return 7; }

namespace {

struct Primary {
    virtual int anchor() { return 9; }
    long padding;
};

struct Complete : Primary, B {};

B* operand;
int evaluations;

bool same_top(B* source, void* expected) {
    return top(source) == expected && top_const(source) == expected &&
           top_volatile(source) == expected && top_cv(source) == expected;
}

bool evaluate_once(B* source, void* expected) {
    operand = source;
    evaluations = 0;
    return top_once() == expected && evaluations == 1;
}

} // namespace

B* take_operand() {
    ++evaluations;
    return evaluations == 1 ? operand : nullptr;
}

int main() {
    B root;
    if (!same_top(&root, &root)) return 11;

    Complete complete;
    B* secondary = &complete;
    if (static_cast<void*>(secondary) == &complete) return 21;
    if (!same_top(secondary, &complete)) return 22;

    if (!same_top(nullptr, nullptr)) return 31;
    if (!evaluate_once(secondary, &complete)) return 41;
    if (!evaluate_once(nullptr, nullptr)) return 42;
}

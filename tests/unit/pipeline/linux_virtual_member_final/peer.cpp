#include "root.hpp"

namespace final_probe {
int B::marker(int value) const {
    return 100 + tag + value;
}

int B::score(int value) const {
    return 200 + tag + value;
}

int D::score(int value) const {
    return 300 + tag + value;
}
}

int main() {
    final_probe::B base;
    final_probe::D derived;
    base.tag = 7;
    derived.tag = 11;
    if (base_slot(&base, 3) != 210)
        return 1;
    if (base_slot(&derived, 3) != 314)
        return 2;
    if (derived_slot(derived, 3) != 314)
        return 3;
    if (qualified_base(derived, 3) != 214)
        return 4;
    if (other_slot(&base, 3) != 110 || other_slot(&derived, 3) != 114)
        return 5;
    return 0;
}

#include "inline_functions.h"

static const Function declared[] = {&inline_first, &inline_later, &Operation::member, &implicit_inline};

static Function volatile stored_first = &inline_first;
static Function volatile stored_later = &inline_later;
static Function volatile stored_member = &Operation::member;
static Function volatile stored_constexpr = &implicit_inline;

Function main_first_address() { return stored_first; }
Function main_later_address() { return stored_later; }
Function main_member_address() { return stored_member; }
Function main_constexpr_address() { return stored_constexpr; }

int main_direct_calls(int value) {
    return inline_first(value) + inline_later(value) +
           Operation::member(value) + implicit_inline(value);
}

int main(int argc, char**) {
    const int value = argc & 127;
    const Function local[] = {main_first_address(), main_later_address(),
                              main_member_address(), main_constexpr_address()};
    const Function peer[] = {peer_first_address(), peer_later_address(),
                             peer_member_address(), peer_constexpr_address()};
    const int expected[] = {value + 11, value + 17, value + 23, value + 29};

    for (int index = 0; index < 4; ++index) {
        if (local[index] == nullptr || peer[index] == nullptr) return 1 + index;
        if (local[index](value) != expected[index] || peer[index](value) != expected[index]) {
            return 5 + index;
        }
        if (local[index] != peer[index] || local[index] != declared[index]) return 9 + index;
    }

    if (main_direct_calls(value) != 4 * value + 80) return 13;
    if (peer_direct_calls(value) != 4 * value + 80) return 14;
    return 0;
}

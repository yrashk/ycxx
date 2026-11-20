#include "inline_functions.h"

static Function volatile stored_first = &inline_first;
static Function volatile stored_later = &inline_later;
static Function volatile stored_member = &Operation::member;
static Function volatile stored_constexpr = &implicit_inline;

Function peer_first_address() { return stored_first; }
Function peer_later_address() { return stored_later; }
Function peer_member_address() { return stored_member; }
Function peer_constexpr_address() { return stored_constexpr; }

int peer_direct_calls(int value) {
    return inline_first(value) + inline_later(value) +
           Operation::member(value) + implicit_inline(value);
}

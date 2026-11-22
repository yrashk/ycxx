#include "root.hpp"
#include <cstdint>
#include <cstring>
#include <typeinfo>

// The symbol carries the C prefix (`_` on Darwin, none on ELF).
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
extern const unsigned char derived_table[24] asm(LABEL_PREFIX(__USER_LABEL_PREFIX__) "_ZTV1D");

static_assert(sizeof(D) == 16 && alignof(D) == 8);

int D::f() {
    return 5 * tag - 3;
}

extern "C" int peer_observe(D& object, B& base, int initial, int updated) noexcept {
    const void* address = &object;
    if (&base != static_cast<B*>(&object) || static_cast<const void*>(&base) != address)
        return -1;
    const void* vptr;
    std::memcpy(&vptr, address, sizeof(vptr));
    if (vptr != derived_table + 16)
        return -2;
    if (&typeid(base) != &typeid(D))
        return -3;
    if (reinterpret_cast<std::uintptr_t>(&object.tag) - reinterpret_cast<std::uintptr_t>(address) != 8)
        return -4;
    if (object.tag != initial || opaque_dispatch(base) != 5 * initial - 3)
        return -5;
    object.tag = updated;
    if (opaque_dispatch(base) != 5 * updated - 3)
        return -6;
    return 0;
}

int main() {
    const int first = make_derived(11, 49);
    if (first != 49)
        return first < 0 ? -first : 7;
    const int second = make_derived(29, 73);
    if (second != 73)
        return second < 0 ? 16 - second : 23;
    return 0;
}

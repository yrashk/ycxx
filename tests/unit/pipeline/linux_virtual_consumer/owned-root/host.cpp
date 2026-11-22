#include "root.hpp"
#include <cstdint>
#include <cstring>
#include <typeinfo>

#ifndef EXPECT_FIRST
#define EXPECT_FIRST 71
#endif

extern const unsigned char root_vtable[32] asm("_ZTVN6abi3094RootE");

namespace abi309 {
static_assert(sizeof(Root) == 16);
static_assert(alignof(Root) == 8);

static int inspect(Root& root) {
    if (reinterpret_cast<std::uintptr_t>(&root.member) -
            reinterpret_cast<std::uintptr_t>(&root) != 8) {
        return 1;
    }
    if (root.member != 29) {
        return 2;
    }
    if (root.first() != EXPECT_FIRST) {
        return 3;
    }
    if (call_second(root) != 83) {
        return 4;
    }
    if (&typeid(root) != &typeid(Root)) {
        return 5;
    }
    if (std::strcmp(typeid(root).name(), "N6abi3094RootE") != 0) {
        return 6;
    }
    if (dynamic_cast<void*>(&root) != static_cast<void*>(&root)) {
        return 7;
    }
    const void* address_point;
    std::memcpy(&address_point, static_cast<const void*>(&root), sizeof(address_point));
    if (address_point != root_vtable + 16) {
        return 8;
    }
    return 0;
}
}

int main() {
    abi309::Root root;
    root.member = 29;
    return abi309::inspect(root);
}

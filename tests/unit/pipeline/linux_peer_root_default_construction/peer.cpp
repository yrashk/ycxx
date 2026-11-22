#include "root.hpp"
#include <cstdint>
#include <cstring>
#include <typeinfo>

#ifndef EXPECT_COUNT
#define EXPECT_COUNT 7
#endif

// The symbol carries the C prefix (`_` on Darwin, none on ELF).
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
extern const unsigned char root_vtable[32] asm(LABEL_PREFIX(__USER_LABEL_PREFIX__) "_ZTVN6abi3094RootE");

static_assert(sizeof(abi309::Root) == 16);
static_assert(alignof(abi309::Root) == 8);

int abi309::peer_check(Root* root) {
    if (root == nullptr)
        return 1;
    const void* address_point;
    std::memcpy(&address_point, static_cast<const void*>(root), sizeof(address_point));
    if (address_point == root_vtable)
        return 91;
    if (address_point != root_vtable + 16)
        return 2;
    if (reinterpret_cast<std::uintptr_t>(&root->count) - reinterpret_cast<std::uintptr_t>(root) != 8)
        return 3;
    if (root->count != EXPECT_COUNT)
        return 4;
    if (root->first() != 11 || root->second() != 23)
        return 5;
    if (&typeid(*root) != &typeid(Root))
        return 6;
    if (std::strcmp(typeid(*root).name(), "N6abi3094RootE") != 0)
        return 7;
    return 0;
}

int main() {
    return abi309::make_root();
}

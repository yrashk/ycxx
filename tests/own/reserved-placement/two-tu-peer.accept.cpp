// REQUIRES: linux
#include "placement.hpp"

bool peer_check() {
    int storage = 17;
    void* pointer = &storage;

    if (::operator new(sizeof(storage), pointer) != pointer)
        return false;
    if (::operator new[](sizeof(storage), pointer) != pointer)
        return false;

    ::operator delete(pointer, pointer);
    if (storage != 17)
        return false;
    ::operator delete[](pointer, pointer);
    return storage == 17;
}

PlacementAddresses peer_addresses() {
    return {&::operator new, &::operator new[], &::operator delete, &::operator delete[]};
}

void* operator new(Size, const AllocationTag& tag) noexcept {
    return tag.pointer;
}

#include "placement.hpp"

int main() {
    int storage = 23;
    void* pointer = &storage;

    if (!peer_check())
        return 1;
    if (::operator new(sizeof(storage), pointer) != pointer)
        return 2;
    if (::operator new[](sizeof(storage), pointer) != pointer)
        return 3;

    ::operator delete(pointer, pointer);
    if (storage != 23)
        return 4;
    ::operator delete[](pointer, pointer);
    if (storage != 23)
        return 5;

    PlacementAddresses local = {&::operator new, &::operator new[],
                                &::operator delete, &::operator delete[]};
    PlacementAddresses peer;
    peer_addresses(peer);
    if (local.scalar_new != peer.scalar_new || local.array_new != peer.array_new)
        return 6;
    if (local.scalar_delete != peer.scalar_delete || local.array_delete != peer.array_delete)
        return 7;

    AllocationTag tag{pointer};
    if (::operator new(sizeof(storage), tag) != pointer)
        return 8;
    return 0;
}

#ifndef PLACEMENT_HPP
#define PLACEMENT_HPP

using Size = decltype(sizeof(0));

[[gnu::visibility("hidden")]]
constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

[[gnu::visibility("hidden")]]
constexpr void* operator new[](Size, void* pointer) noexcept {
    return pointer;
}

[[gnu::visibility("hidden")]]
constexpr void operator delete(void*, void*) noexcept {}

[[gnu::visibility("hidden")]]
constexpr void operator delete[](void*, void*) noexcept {}

using Allocate = void* (*)(Size, void*) noexcept;
using Deallocate = void (*)(void*, void*) noexcept;

struct PlacementAddresses {
    Allocate scalar_new;
    Allocate array_new;
    Deallocate scalar_delete;
    Deallocate array_delete;
};

struct AllocationTag {
    void* pointer;
};

void* operator new(Size, const AllocationTag& tag) noexcept;
bool peer_check();
void peer_addresses(PlacementAddresses& addresses);

#endif

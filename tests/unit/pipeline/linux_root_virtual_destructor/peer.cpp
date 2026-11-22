#include "root.hpp"

#include <new>

namespace {

constexpr int root_key = 11;
constexpr int derived_key = 29;
constexpr auto alignment = std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__};

struct Events {
    R* expected = nullptr;
    int observations = 0;
    int key = 0;
    int derived_at_observation = 0;
    int sized_deletes = 0;
    int unsized_deletes = 0;
    std::size_t size = 0;
    bool observer_received_original = false;
    bool delete_received_original = false;
    bool delete_followed_observation = false;
};

Events events;
int derived_destructions;
void* allocation;

struct Derived : R {
    int key() noexcept override { return derived_key; }
    ~Derived() noexcept override { ++derived_destructions; }
};

bool observed_root() noexcept {
    return events.observations == 1 && events.observer_received_original && events.key == root_key;
}

bool no_deallocation() noexcept {
    return events.sized_deletes == 0 && events.unsized_deletes == 0;
}

void deallocate(void* pointer) noexcept {
    events.delete_received_original = allocation != nullptr && pointer == allocation;
    events.delete_followed_observation = observed_root();
    if (events.delete_received_original) {
        allocation = nullptr;
        events.expected = nullptr;
        ::operator delete(pointer, alignment);
    }
}

} // namespace

int R::key() noexcept { return root_key; }

void observe(R* root) noexcept {
    ++events.observations;
    events.observer_received_original = root == events.expected;
    events.derived_at_observation = derived_destructions;
    if (events.observer_received_original) events.key = call_key(root);
}

void* operator new(std::size_t size) {
    // Pair the replacement ordinary allocation with the library's aligned allocation and deletion.
    allocation = ::operator new(size, alignment);
    return allocation;
}

void operator delete(void* pointer, std::size_t size) noexcept {
    ++events.sized_deletes;
    events.size = size;
    deallocate(pointer);
}

void operator delete(void* pointer) noexcept {
    ++events.unsized_deletes;
    deallocate(pointer);
}

int main() {
    // The native caller destroys a complete automatic object through D1.
    {
        R root;
        events.expected = &root;
    }
    if (!observed_root() || !no_deallocation()) return 11;

    events = {};
    // The opaque delete dispatches through the native table to D0.
    R* heap = new R;
    if (heap != allocation) return 21;
    events.expected = heap;
    destroy(heap);
    if (!observed_root()) return 22;
    if (events.sized_deletes != 1 || events.unsized_deletes != 0) return 23;
    if (events.size != sizeof(R) || !events.delete_received_original ||
        !events.delete_followed_observation) return 24;

    events = {};
    // The native derived destructor calls D2 while the derived table would yield a different key.
    {
        Derived derived;
        events.expected = &derived;
        if (call_key(&derived) != derived_key) return 31;
    }
    if (!observed_root() || !no_deallocation()) return 32;
    if (derived_destructions != 1 || events.derived_at_observation != 1) return 33;
}

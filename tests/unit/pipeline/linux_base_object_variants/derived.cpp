#include "trace.hpp"

namespace {
struct Derived : p::Base {
    Derived(int initial) noexcept : p::Base(initial) {
        record_base_event(2, this);
    }

    ~Derived() noexcept {
        record_base_event(3, this);
    }

    virtual int read() const noexcept {
        return value + 5;
    }
};
}

int probe_derived() noexcept {
    reset_trace();
    std::uintptr_t base;
    {
        Derived object(41);
        base = reinterpret_cast<std::uintptr_t>(static_cast<p::Base*>(&object));
        if (base - reinterpret_cast<std::uintptr_t>(&object) != 8)
            return 1;
        if (object.value != 41 || object.read() != 46)
            return 2;
        object.value = 52;
    }
    const Event expected[] = {{1, 41}, {2, 41}, {3, 52}, {4, 52}};
    return trace_matches(base, expected, 4) ? 0 : 3;
}

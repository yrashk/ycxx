#include "trace.hpp"

int probe_direct() noexcept {
    reset_trace();
    std::uintptr_t base;
    {
        p::Base object(17);
        base = reinterpret_cast<std::uintptr_t>(&object);
        if (object.value != 17)
            return 4;
        object.value = 23;
    }
    const Event expected[] = {{1, 17}, {4, 23}};
    return trace_matches(base, expected, 2) ? 0 : 5;
}

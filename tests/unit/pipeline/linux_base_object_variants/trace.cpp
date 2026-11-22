#include "trace.hpp"

namespace {
struct Observation {
    std::uintptr_t address;
    Event event;
};

Observation observed[4];
unsigned used;
}

extern "C" void record_base_event(int phase, const p::Base* object) noexcept {
    if (used < 4)
        observed[used] = {reinterpret_cast<std::uintptr_t>(object), {phase, object->value}};
    ++used;
}

void reset_trace() noexcept {
    used = 0;
}

bool trace_matches(std::uintptr_t base, const Event* expected, unsigned count) noexcept {
    if (used != count)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        if (observed[i].address != base || observed[i].event.phase != expected[i].phase ||
            observed[i].event.value != expected[i].value)
            return false;
    }
    return true;
}

int main() {
    const int derived = probe_derived();
    return derived == 0 ? probe_direct() : derived;
}

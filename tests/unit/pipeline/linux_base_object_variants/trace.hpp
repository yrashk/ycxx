#ifndef ABI136_TRACE_HPP
#define ABI136_TRACE_HPP

#include "base.hpp"
#include <cstdint>

struct Event {
    int phase;
    int value;
};

void reset_trace() noexcept;
bool trace_matches(std::uintptr_t base, const Event* expected, unsigned count) noexcept;
int probe_derived() noexcept;
int probe_direct() noexcept;

#endif

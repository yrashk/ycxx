// REQUIRES: linux
// PARITY: by design: this include order exercises glibc's integer headers on Linux
#include <cstdint>
#include <cinttypes>

static_assert(INT8_MIN == -128 && UINT64_C(42) == 42);
static_assert(sizeof(std::int_fast16_t) == sizeof(long));

// REQUIRES: linux
// PARITY: by design: this include order exercises glibc's integer headers on Linux
#include <inttypes.h>
#include <cstdint>

static_assert(INT8_MIN == -128 && UINT64_C(42) == 42);
static_assert(__is_same(std::int_fast16_t, ::int_fast16_t));

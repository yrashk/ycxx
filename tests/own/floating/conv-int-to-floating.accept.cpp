// [conv.fpint]/2: an integer converts to a floating type exactly when it can; otherwise to the next
// lower or higher value (implementation-defined; the target rounds to nearest, ties to even).
static_assert(static_cast<double>(16777217) == 16777217.0);
static_assert(static_cast<float>(16777217) == 16777216.0f);
static_assert(static_cast<float>(16777219) == 16777220.0f);
static_assert(static_cast<double>(9007199254740993LL) == 9007199254740992.0);
static_assert(static_cast<double>(9007199254740995LL) == 9007199254740996.0);
static_assert(static_cast<float>(4294967295u) == 4294967296.0f);
static_assert(static_cast<double>(4294967295u) == 4294967295.0);
static_assert(static_cast<double>(-2147483647 - 1) == -2147483648.0);
static_assert(static_cast<double>(18446744073709551615ULL) == 18446744073709551616.0);
static_assert(static_cast<double>(-9223372036854775807LL - 1) == -0x1p63);
static_assert(static_cast<float>('A') == 65.0f);
static_assert(static_cast<double>(-1) == -1.0);
static_assert(static_cast<double>(0) == 0.0);

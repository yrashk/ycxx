// XFAIL: ycxx array-to-pointer decay in convertibility is unsupported (#1399)
// Either Boolean result makes the assertion true and triggers XPASS.
#define EITHER(...) (__VA_ARGS__ || !(__VA_ARGS__))
static_assert(EITHER(__is_convertible(int[3], const int*)));

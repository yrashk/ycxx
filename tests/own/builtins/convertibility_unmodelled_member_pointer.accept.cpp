// XFAIL: ycxx qualification through a pointer to a member pointer is unsupported (#1399)
// Either Boolean result makes the assertion true and triggers XPASS.
struct C {};
#define EITHER(...) (__VA_ARGS__ || !(__VA_ARGS__))
static_assert(EITHER(__is_convertible(int C::**, const int C::* const*)));

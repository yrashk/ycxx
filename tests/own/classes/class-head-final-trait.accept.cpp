class Closed final {};
struct Open {};

static_assert(__is_final(Closed));
static_assert(__is_final(const volatile Closed));
static_assert(!__is_final(Open));

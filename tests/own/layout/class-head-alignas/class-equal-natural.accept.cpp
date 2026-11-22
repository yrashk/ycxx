// [dcl.align] Class-head alignment.
struct Plain { long long value; };
struct alignas(alignof(Plain)) Same { long long value; };

static_assert(alignof(Same) == alignof(Plain));
static_assert(sizeof(Same) == sizeof(Plain));

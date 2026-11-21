// [dcl.align] Class-head alignment.
// XFAIL: gcc GCC 16.2 gives Raised and Reverse alignment 8. [dcl.align] requires 16.
struct alignas(1) alignas(16) alignas(8) Raised { long long value; };
struct alignas(16) alignas(1) Reverse { long long value; };
struct alignas(8 + 8) alignas(16) Duplicate { char value; };

static_assert(alignof(Raised) == 16);
static_assert(alignof(Reverse) == 16);
static_assert(alignof(Duplicate) == 16);

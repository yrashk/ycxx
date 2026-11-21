// [dcl.align] Class-head alignment.
struct alignas(16) Announced;
struct Announced;
struct alignas(16) Announced { char value; };
struct Announced;

static_assert(alignof(Announced) == 16);

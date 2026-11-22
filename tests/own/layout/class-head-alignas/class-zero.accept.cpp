// [dcl.align] Class-head alignment.
struct Plain { long long value; };
struct alignas(0) Unchanged { long long value; };
struct alignas(0) alignas(16) Raised { char value; };

static_assert(alignof(Unchanged) == alignof(Plain));
static_assert(sizeof(Unchanged) == sizeof(Plain));
static_assert(alignof(Raised) == 16);

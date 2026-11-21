// [class.pre], [dcl.align] Attributes in a class-head.
struct alignas(1) [[maybe_unused]] alignas(16) Interleaved { char value; };

static_assert(alignof(Interleaved) == 16);
static_assert(sizeof(Interleaved) == 16);

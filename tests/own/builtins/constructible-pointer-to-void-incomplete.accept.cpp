// A pointer to an incomplete class converts to a pointer to void ([conv.ptr]/2), so
// direct-initialization is valid without the class definition ([meta.unary.prop]).
struct Incomplete;
static_assert(__is_constructible(void*, Incomplete*));
static_assert(__is_constructible(const void*, Incomplete*&));
static_assert(!__is_constructible(Incomplete*, void*));

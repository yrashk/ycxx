// #218: A pointer operand does not require its pointee to be complete.
struct Forward;
static_assert(__is_trivially_copyable(Forward*));

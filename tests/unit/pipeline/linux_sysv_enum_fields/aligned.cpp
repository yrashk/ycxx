enum class [[gnu::aligned(16)]] E : int { one = 1 };
struct R { E code; char tail; };
#ifdef VERIFY_ENUM_LAYOUT
static_assert(alignof(E) == 16);
static_assert(alignof(R) == 16);
#endif
R send(char tail) { return {E::one, tail}; }

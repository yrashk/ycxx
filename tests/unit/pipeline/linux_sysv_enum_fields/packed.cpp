enum __attribute__((packed)) E { one = 1 };
struct R { E code; char tail; };
#ifdef VERIFY_ENUM_LAYOUT
static_assert(sizeof(E) == 1);
static_assert(sizeof(R) == 2);
#endif
R send(char tail) { return {one, tail}; }

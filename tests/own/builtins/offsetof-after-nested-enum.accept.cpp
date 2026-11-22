// #248, #220: offsetof of members declared by the same member-declaration as a nested enum
// definition, and of members after it ([class.mem.general], [dcl.enum], [support.types.layout]).
// N has no fixed type and its values fit in an int, so it is 4 bytes on every supported target;
// long is 8 bytes on all of them.
struct S {
  enum N { n0, n1 = 5 } m, list[3];
  char c;
  enum class K : short { k0 } k;
  long tail;
};

static_assert(__builtin_offsetof(S, m) == 0);
static_assert(__builtin_offsetof(S, list) == 4);
static_assert(__builtin_offsetof(S, c) == 16);
static_assert(__builtin_offsetof(S, k) == 18);
static_assert(__builtin_offsetof(S, tail) == 24);
static_assert(sizeof(S) == 32);

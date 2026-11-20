// #248: use offsetof from the pinned libycxx <cstddef> ([support.types.layout]).
// FLAGS: -isystem ../../../deps/libycxx/include
#include <cstddef>

struct Packet {
  char marker;
  int payload;
};

static_assert(offsetof(Packet, marker) == 0);
static_assert(offsetof(Packet, payload) == alignof(int));
static_assert(__is_same(decltype(offsetof(Packet, payload)), std::size_t));

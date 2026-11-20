// #248: byte offsets of direct members of a standard-layout aggregate
// ([support.types.layout]). The supported targets align the long past the first byte.
struct Record {
  char tag;
  long payload;
  char trailer;
};

constexpr auto payload_offset = __builtin_offsetof(Record, payload);
static_assert(__builtin_offsetof(Record, tag) == 0);
static_assert(payload_offset == alignof(long));
static_assert(__builtin_offsetof(Record, trailer) == payload_offset + sizeof(long));
static_assert(__is_same(decltype(__builtin_offsetof(Record, payload)), decltype(sizeof(0))));

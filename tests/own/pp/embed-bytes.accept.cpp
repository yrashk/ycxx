// [cpp.embed.gen] and [cpp.embed.param]: expand binary bytes and apply the supplied parameters.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

constexpr bool is_resource(int a, int b, int c, int d) {
  return a == 0 && b == 255 && c == 65 && d == 13;
}

static_assert(is_resource(
#embed "inputs/embed-bytes.bin"
));

#define RESOURCE "inputs/embed-bytes.bin"
#define COUNT (1 + 1)
#define START (17),
#define END ,(19)
constexpr bool is_limited(int a, int b, int c, int d) {
  return a == 17 && b == 0 && c == 255 && d == 19;
}

static_assert(is_limited(
#embed RESOURCE suffix(END) if_empty(-1) limit(COUNT) prefix(START)
));

constexpr int empty =
#embed "inputs/embed-empty.bin" prefix(17,) suffix(,19) if_empty(23)
;
static_assert(empty == 23);

constexpr int discarded =
#embed RESOURCE limit(0) prefix(17,) suffix(,19)
31;
static_assert(discarded == 31);

static_assert(is_resource(
#embed RESOURCE limit(100) prefix() suffix() if_empty()
));

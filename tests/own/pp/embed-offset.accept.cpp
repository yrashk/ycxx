// [cpp.embed.gen] and [cpp.embed.param.offset]: skip bytes before applying the count limit.
// XFAIL: clang,gcc the compilers only implement offset as a vendor-prefixed parameter
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

#define RESOURCE "inputs/embed-bytes.bin"
constexpr bool is_tail(int a, int b) {
  return a == 65 && b == 13;
}

static_assert(is_tail(
#embed RESOURCE offset(1 + 1)
));

constexpr int slice =
#embed RESOURCE limit(1) offset(1)
;
static_assert(slice == 255);

constexpr int past_end =
#embed RESOURCE offset(99) if_empty(17)
;
static_assert(past_end == 17);

#if __has_embed(RESOURCE offset(3) limit(8)) != __STDC_EMBED_FOUND__
#error "the final byte is available"
#endif
#if __has_embed(RESOURCE offset(4)) != __STDC_EMBED_EMPTY__
#error "an offset at the end makes the resource empty"
#endif

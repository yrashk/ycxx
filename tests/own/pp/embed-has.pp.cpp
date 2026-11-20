// [cpp.cond] and [cpp.predefined]: distinguish available, empty and unavailable resources.
#ifdef __has_embed
embed_is_defined;
#endif
#if defined(__has_embed)
embed_defined_operator;
#endif
__STDC_EMBED_NOT_FOUND__ __STDC_EMBED_FOUND__ __STDC_EMBED_EMPTY__
#define RESOURCE "inputs/embed-bytes.bin"
#define ARGS limit(UNKNOWN_COUNT) prefix(([{}])) suffix() if_empty(23)
#if __has_embed(RESOURCE limit(1) prefix(4,) suffix(,5) if_empty(6)) == __STDC_EMBED_FOUND__
nonempty;
#endif
#if __has_embed("inputs/embed-empty.bin") == __STDC_EMBED_EMPTY__
empty_file;
#endif
#if __has_embed(RESOURCE ARGS) == __STDC_EMBED_EMPTY__
zero_limit;
#endif
#if __has_embed("inputs/no-such-embed.bin" limit(0)) == __STDC_EMBED_NOT_FOUND__
missing_file;
#endif
#if __has_embed(RESOURCE unknown_vendor::parameter({[()]})) == __STDC_EMBED_NOT_FOUND__
unsupported_parameter;
#endif

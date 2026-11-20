// [cpp.embed.gen]/6: a missing resource is ill-formed even with a zero limit.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#embed "inputs/no-such-embed.bin" limit(0) //! error

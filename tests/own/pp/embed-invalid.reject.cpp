// [cpp.embed.param]: invalid parameters are diagnosed.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#embed "inputs/embed-bytes.bin" limit(-1) //! error
#embed "inputs/embed-bytes.bin" limit(1) limit(2) //! error
#embed "inputs/embed-bytes.bin" suffix() suffix() //! error
#embed "inputs/embed-bytes.bin" limit() //! error

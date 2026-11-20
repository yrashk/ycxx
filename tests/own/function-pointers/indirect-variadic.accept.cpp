// [expr.call], [conv.prom]
using Variadic = int (*)(int, ...);
using Plain = int (*)(int);

extern "C" int indirect_variadic_empty(Variadic target, int tag) {
    return target(tag);
}

extern "C" int indirect_variadic_values(Variadic target, signed char small,
                                       unsigned short large, bool flag,
                                       int* object, signed short negative,
                                       unsigned char byte, Plain callback) {
    return (*target)(23, small, large, flag, object, 0x123456789UL,
                     negative, byte, callback);
}

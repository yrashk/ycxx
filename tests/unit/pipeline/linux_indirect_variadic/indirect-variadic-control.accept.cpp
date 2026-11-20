// [expr.call], [conv.prom]
extern "C" int variadic_values(int, ...);
using Plain = int (*)(int);

extern "C" int direct_variadic_control(signed char value) {
    return variadic_values(5, value);
}

extern "C" int indirect_fixed_control(Plain target, int value) {
    return target(value);
}

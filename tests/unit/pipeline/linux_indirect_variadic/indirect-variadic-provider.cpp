#include <stdarg.h>

using Variadic = int (*)(int, ...);
using Plain = int (*)(int);

extern "C" int indirect_variadic_empty(Variadic, int);
extern "C" int indirect_variadic_values(Variadic, signed char, unsigned short,
                                       bool, int*, signed short,
                                       unsigned char, Plain);
extern "C" int direct_variadic_control(signed char);
extern "C" int indirect_fixed_control(Plain, int);

extern "C" int indirect_variadic_seed_al(Variadic, int);
extern "C" int indirect_variadic_read_al(int, ...);
extern "C" int indirect_variadic_check_al(int, ...);

namespace {
int identity(int value) {
    return value;
}

int object = 73;
}

extern "C" int indirect_variadic_receive(int tag, ...) {
    va_list arguments;
    va_start(arguments, tag);
    const int small = va_arg(arguments, int);
    const int large = va_arg(arguments, int);
    const int flag = va_arg(arguments, int);
    int* const pointer = va_arg(arguments, int*);
    const unsigned long wide = va_arg(arguments, unsigned long);
    const int negative = va_arg(arguments, int);
    const int byte = va_arg(arguments, int);
    const Plain callback = va_arg(arguments, Plain);
    va_end(arguments);

    if (tag != 23 || small != -61 || large != 60000 || flag != 1)
        return 1;
    if (pointer != &object || wide != 0x123456789UL)
        return 2;
    if (negative != -1234 || byte != 211 || callback != identity)
        return 3;
    return 0;
}

extern "C" int variadic_values(int tag, ...) {
    va_list arguments;
    va_start(arguments, tag);
    const int value = va_arg(arguments, int);
    va_end(arguments);
    return tag == 5 && value == -61 ? 31 : 0;
}

int main() {
    if (indirect_variadic_seed_al(indirect_variadic_read_al, 47) != 47)
        return 4;
    if (const int mismatch = indirect_variadic_values(indirect_variadic_check_al, -61,
                                                       60000, true, &object,
                                                       -1234, 211, identity))
        return mismatch;
    if (direct_variadic_control(-61) != 31)
        return 5;
    if (indirect_fixed_control(identity, 79) != 79)
        return 6;
    return 0;
}

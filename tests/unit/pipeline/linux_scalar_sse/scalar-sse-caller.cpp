#include "scalar-sse.h"

extern "C" void exercise_widths(float fzero, double dzero, float* singles, double* doubles) {
    float single = abi_binary32_constant();
    double wide = abi_binary64_constant();
    singles[0] = single;
    doubles[0] = wide;
    singles[2] = abi_binary32_identity(singles[0]);
    doubles[2] = abi_binary64_identity(doubles[0]);
    singles[4] = abi_binary32_identity(fzero);
    doubles[4] = abi_binary64_identity(dzero);
}

extern "C" void exercise_integer_first(unsigned long* integers, float* singles, double* doubles) {
    abi_integer_first(
        integers, singles, doubles,
        0x1100000000000001UL, 11.25F, 0x2200000000000002UL, 12.625,
        0x3300000000000003UL, 13.5F, 0x4400000000000004UL, 14.875,
        15.75F, 16.0625, 17.125F, 18.3125,
        19.25F, 0x5500000000000005UL, 20.5625, 21.5F);
}

extern "C" void exercise_sse_first(unsigned long* integers, float* singles, double* doubles) {
    abi_sse_first(
        integers, singles, doubles,
        31.0625, 0x6600000000000006UL, 32.5F, 33.3125, 34.75F,
        0x7700000000000007UL, 35.5625, 36.125F, 37.8125, 38.25F,
        39.4375, 0x8800000000000008UL, 0x9900000000000009UL, 40.5F,
        0xaa0000000000000aUL);
}

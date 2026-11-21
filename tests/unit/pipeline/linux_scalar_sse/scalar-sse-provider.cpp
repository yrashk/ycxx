#include "scalar-sse.h"

extern "C" float abi_binary32_constant() {
    return 0x1.000002p0F;
}

extern "C" double abi_binary64_constant() {
    return 0x1.0000000000001p0;
}

extern "C" float abi_binary32_identity(float value) {
    return value;
}

extern "C" double abi_binary64_identity(double value) {
    return value;
}

extern "C" void abi_integer_first(
    unsigned long* integers, float* singles, double* doubles,
    unsigned long i0, float f0, unsigned long i1, double d0,
    unsigned long i2, float f1, unsigned long i3, double d1,
    float f2, double d2, float f3, double d3,
    float f4, unsigned long i4, double d4, float f5) {
    integers[0] = i0;
    integers[1] = i1;
    integers[2] = i2;
    integers[3] = i3;
    integers[4] = i4;
    singles[0] = f0;
    singles[1] = f1;
    singles[2] = f2;
    singles[3] = f3;
    singles[4] = f4;
    singles[5] = f5;
    doubles[0] = d0;
    doubles[1] = d1;
    doubles[2] = d2;
    doubles[3] = d3;
    doubles[4] = d4;
}

extern "C" void abi_sse_first(
    unsigned long* integers, float* singles, double* doubles,
    double d0, unsigned long i0, float f0, double d1, float f1,
    unsigned long i1, double d2, float f2, double d3, float f3,
    double d4, unsigned long i2, unsigned long i3, float f4, unsigned long i4) {
    integers[0] = i0;
    integers[1] = i1;
    integers[2] = i2;
    integers[3] = i3;
    integers[4] = i4;
    singles[0] = f0;
    singles[1] = f1;
    singles[2] = f2;
    singles[3] = f3;
    singles[4] = f4;
    doubles[0] = d0;
    doubles[1] = d1;
    doubles[2] = d2;
    doubles[3] = d3;
    doubles[4] = d4;
}

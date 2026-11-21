#ifndef YCXX_SCALAR_SSE_FIXTURE_H
#define YCXX_SCALAR_SSE_FIXTURE_H

extern "C" float abi_binary32_constant();
extern "C" double abi_binary64_constant();
extern "C" float abi_binary32_identity(float value);
extern "C" double abi_binary64_identity(double value);

extern "C" void abi_integer_first(
    unsigned long* integers, float* singles, double* doubles,
    unsigned long i0, float f0, unsigned long i1, double d0,
    unsigned long i2, float f1, unsigned long i3, double d1,
    float f2, double d2, float f3, double d3,
    float f4, unsigned long i4, double d4, float f5);

extern "C" void abi_sse_first(
    unsigned long* integers, float* singles, double* doubles,
    double d0, unsigned long i0, float f0, double d1, float f1,
    unsigned long i1, double d2, float f2, double d3, float f3,
    double d4, unsigned long i2, unsigned long i3, float f4, unsigned long i4);

extern "C" void exercise_widths(float fzero, double dzero, float* singles, double* doubles);
extern "C" void exercise_integer_first(unsigned long* integers, float* singles, double* doubles);
extern "C" void exercise_sse_first(unsigned long* integers, float* singles, double* doubles);

#endif

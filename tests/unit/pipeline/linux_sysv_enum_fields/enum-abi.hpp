#pragma once

namespace abi1682 {

enum class Code : int { ok = 0, invalid = 23, negative = -1234567 };
enum class Narrow : signed char { value = -91 };
enum class Wide : unsigned long long { value = 0xfedcba9876543210ULL };

struct Result { char* ptr; Code ec; };
struct NarrowResult { char* ptr; Narrow ec; };
struct WideResult { char* ptr; Wide ec; };

static_assert(sizeof(char*) == 8 && alignof(char*) == 8);
static_assert(sizeof(Code) == 4 && alignof(Code) == 4);
static_assert(sizeof(Narrow) == 1 && alignof(Narrow) == 1);
static_assert(sizeof(Wide) == 8 && alignof(Wide) == 8);
static_assert(sizeof(Result) == 16 && alignof(Result) == 8);
static_assert(sizeof(NarrowResult) == 16 && alignof(NarrowResult) == 8);
static_assert(sizeof(WideResult) == 16 && alignof(WideResult) == 8);

inline constexpr unsigned long long cookie = 0xd5c4317290febc86ULL;

Result make_result(char* address, Code code);
int take_result(Result input, unsigned long long tail, char* expected);
int take_spilled(unsigned long long a, unsigned long long b, unsigned long long c,
                 unsigned long long d, unsigned long long e, Result input,
                 unsigned long long tail, char* expected);

NarrowResult make_narrow(char* address, Narrow code);
int take_narrow(NarrowResult input, unsigned long long tail, char* expected);
WideResult make_wide(char* address, Wide code);
int take_wide(WideResult input, unsigned long long tail, char* expected);

}

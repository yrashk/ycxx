#define DEFINE_FP_COMPARISONS(Suffix, Type)                                  \
  extern "C" void fp_values_##Suffix(Type a, Type b, int* values) {           \
    values[0] = a < b;                                                       \
    values[1] = a <= b;                                                      \
    values[2] = a > b;                                                       \
    values[3] = a >= b;                                                      \
    values[4] = a == b;                                                      \
    values[5] = a != b;                                                      \
  }                                                                         \
  extern "C" int fp_branches_##Suffix(Type a, Type b) {                       \
    int result = 0;                                                         \
    if (a < b) result |= 1 << 0;                                             \
    if (a <= b) result |= 1 << 1;                                            \
    if (a > b) result |= 1 << 2;                                             \
    if (a >= b) result |= 1 << 3;                                            \
    if (a == b) result |= 1 << 4;                                            \
    if (a != b) result |= 1 << 5;                                            \
    return result;                                                          \
  }                                                                         \
  extern "C" int fp_bool_##Suffix(Type value) {                              \
    bool result = value;                                                    \
    return result;                                                          \
  }                                                                         \
  extern "C" int fp_context_##Suffix(Type value) {                           \
    if (value) return 17;                                                    \
    return 29;                                                              \
  }

DEFINE_FP_COMPARISONS(32, float)
DEFINE_FP_COMPARISONS(64, double)

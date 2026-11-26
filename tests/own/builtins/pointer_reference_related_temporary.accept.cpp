// XFAIL: clang Clang 23 binds a similar pointer xvalue to a converted temporary, which [dcl.init.ref]/5.4 forbids since CWG2352
// #1396: T1 reference-related to T2 (similar, [conv.qual]) without reference compatibility, so
// no direct binding (/5.3) and no temporary either (/5.4: "T1 shall not be reference-related to
// T2"). GCC 16 agrees.
static_assert(!__is_convertible(int*, const int*&&));
static_assert(!__is_convertible(int**, const int* const*&&));
static_assert(!__is_convertible(int**, const int**&&) && !__is_nothrow_convertible(int*, const int*&&));
// Reference-compatible ones bind directly (/5.3).
static_assert(__is_convertible(int*, const int* const&&) && __is_convertible(int**, const int* const* const&&));

int main() {}

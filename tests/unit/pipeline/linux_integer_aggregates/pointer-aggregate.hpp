#ifndef ABI_POINTER_AGGREGATE_HPP
#define ABI_POINTER_AGGREGATE_HPP

struct PointerOnly {
    int* address;
};

struct PointerCount {
    int* address;
    unsigned long count;
};

struct CountPointer {
    unsigned long count;
    int* address;
};

static_assert(sizeof(PointerOnly) == 8 && alignof(PointerOnly) == 8);
static_assert(sizeof(PointerCount) == 16 && alignof(PointerCount) == 8);
static_assert(sizeof(CountPointer) == 16 && alignof(CountPointer) == 8);

PointerOnly direct_pointer(PointerOnly record, int* replacement);
PointerCount direct_pair(PointerCount record, int* replacement);
CountPointer direct_reverse(CountPointer record, int* replacement);
PointerCount after_five(int a, int b, int c, int d, int e, PointerCount record, int later,
                        int* replacement);
PointerOnly after_six(int a, int b, int c, int d, int e, int f, PointerOnly record, int later,
                      int* replacement);

bool pointer_source_matches(const PointerOnly* source, int* address);
bool pair_source_matches(const PointerCount* source, int* address, unsigned long count);
bool reverse_source_matches(const CountPointer* source, int* address, unsigned long count);

#endif

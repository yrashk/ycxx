#include "pointer-aggregate.hpp"

PointerOnly direct_pointer(PointerOnly record, int* replacement) {
    *record.address += 2;
    record.address = replacement;
    *record.address += 4;
    return record;
}

PointerCount direct_pair(PointerCount record, int* replacement) {
    *record.address += static_cast<int>(record.count & 0xFFUL);
    record.count += 0x100000003UL;
    record.address = replacement;
    *record.address += 6;
    return record;
}

CountPointer direct_reverse(CountPointer record, int* replacement) {
    *record.address += static_cast<int>(record.count & 0xFFUL);
    record.count += 0x200000001UL;
    record.address = replacement;
    *record.address += 8;
    return record;
}

PointerCount after_five(int a, int b, int c, int d, int e, PointerCount record, int later,
                        int* replacement) {
    *record.address += a + 2 * b + 3 * c + 4 * d + 5 * e + later;
    record.count += 0x100000000UL + static_cast<unsigned long>(later);
    record.address = replacement;
    *record.address += 9;
    return record;
}

PointerOnly after_six(int a, int b, int c, int d, int e, int f, PointerOnly record, int later,
                      int* replacement) {
    *record.address += a + b + c + d + e + f + later;
    record.address = replacement;
    *record.address += 11;
    return record;
}

bool pointer_source_matches(const PointerOnly* source, int* address) {
    return source->address == address;
}

bool pair_source_matches(const PointerCount* source, int* address, unsigned long count) {
    return source->address == address && source->count == count;
}

bool reverse_source_matches(const CountPointer* source, int* address, unsigned long count) {
    return source->address == address && source->count == count;
}

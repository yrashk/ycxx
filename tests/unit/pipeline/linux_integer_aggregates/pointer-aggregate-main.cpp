#include "pointer-aggregate.hpp"

bool check_direct_pointer() {
    int first = 13;
    int second = 31;
    PointerOnly source;
    source.address = &first;

    PointerOnly result = direct_pointer(source, &second);
    return pointer_source_matches(&source, &first) && result.address == &second &&
           first == 15 && second == 35;
}

bool check_direct_pair() {
    int first = 17;
    int second = 31;
    PointerCount source;
    source.address = &first;
    source.count = 0x10000002BUL;

    PointerCount result = direct_pair(source, &second);
    return pair_source_matches(&source, &first, 0x10000002BUL) && result.address == &second &&
           result.count == 0x20000002EUL && first == 60 && second == 37;
}

bool check_direct_reverse() {
    int first = 19;
    int second = 41;
    CountPointer source;
    source.count = 0x100000017UL;
    source.address = &first;

    CountPointer result = direct_reverse(source, &second);
    return reverse_source_matches(&source, &first, 0x100000017UL) && result.address == &second &&
           result.count == 0x300000018UL && first == 42 && second == 49;
}

bool check_rollback() {
    int first = 23;
    int second = 47;
    PointerCount source;
    source.address = &first;
    source.count = 0x200000019UL;

    PointerCount result = after_five(2, 3, 5, 7, 11, source, 13, &second);
    return pair_source_matches(&source, &first, 0x200000019UL) && result.address == &second &&
           result.count == 0x300000026UL && first == 142 && second == 56;
}

bool check_exhaustion() {
    int first = 29;
    int second = 53;
    PointerOnly source;
    source.address = &first;

    PointerOnly result = after_six(1, 2, 3, 4, 5, 6, source, 17, &second);
    return pointer_source_matches(&source, &first) && result.address == &second &&
           first == 67 && second == 64;
}

int main() {
    if (!check_direct_pointer()) {
        return 1;
    }
    if (!check_direct_pair()) {
        return 2;
    }
    if (!check_direct_reverse()) {
        return 3;
    }
    if (!check_rollback()) {
        return 4;
    }
    return check_exhaustion() ? 0 : 5;
}

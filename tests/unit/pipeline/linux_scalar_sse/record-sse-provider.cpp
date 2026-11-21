#include "record-sse.h"

extern "C" double abi_spilled_record(
    long a, long b, long c, long d, long e, RecordSsePair record, double value, long f) {
    if (a == 11L && b == 23L && c == 37L && d == 41L && e == 59L &&
        record.first == 101L && record.second == 103L && f == 61L) {
        return value;
    }
    return 0.0;
}

#include "record-sse.h"

extern "C" double exercise_record_sse(const RecordSsePair* record, double value) {
    return abi_spilled_record(11L, 23L, 37L, 41L, 59L, *record, value, 61L);
}

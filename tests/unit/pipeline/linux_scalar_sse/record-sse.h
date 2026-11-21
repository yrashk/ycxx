#ifndef YCXX_RECORD_SSE_FIXTURE_H
#define YCXX_RECORD_SSE_FIXTURE_H

struct RecordSsePair {
    long first;
    long second;
};

extern "C" double abi_spilled_record(
    long a, long b, long c, long d, long e, RecordSsePair record, double value, long f);

extern "C" double exercise_record_sse(const RecordSsePair* record, double value);

#endif

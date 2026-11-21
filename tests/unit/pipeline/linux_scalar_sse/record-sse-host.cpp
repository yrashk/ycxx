#include "record-sse.h"

int main() {
    constexpr double expected = 0x1.0000000000001p0;
    const RecordSsePair valid{101L, 103L};
    const RecordSsePair bad_first{107L, 103L};
    const RecordSsePair bad_second{101L, 109L};
    if (exercise_record_sse(&valid, expected) != expected) {
        return 1;
    }
    if (exercise_record_sse(&bad_first, expected) != 0.0) {
        return 2;
    }
    if (exercise_record_sse(&bad_second, expected) != 0.0) {
        return 3;
    }
    return 0;
}

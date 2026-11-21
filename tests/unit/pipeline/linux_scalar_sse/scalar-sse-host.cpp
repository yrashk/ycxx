#include "scalar-sse.h"

namespace {

template<class Value, class Bits>
bool has_bits(const Value& value, Bits bits) {
    static_assert(sizeof(value) == sizeof(bits));
    const auto* actual = reinterpret_cast<const unsigned char*>(&value);
    const auto* expected = reinterpret_cast<const unsigned char*>(&bits);
    for (unsigned offset = 0; offset < sizeof(value); ++offset) {
        if (actual[offset] != expected[offset]) {
            return false;
        }
    }
    return true;
}

template<class T, unsigned Count>
bool matches(const T (&actual)[Count], const T (&expected)[Count]) {
    for (unsigned index = 0; index < Count; ++index) {
        if (actual[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

int check_widths() {
    float singles[] = {7.25F, 51.25F, 7.25F, 52.5F, 7.25F, 53.75F};
    double doubles[] = {9.5, 61.625, 9.5, 62.875, 9.5, 63.0625};
    exercise_widths(-0.0F, -0.0, singles, doubles);
    if (!has_bits(singles[0], 0x3f800001U) ||
        !has_bits(singles[2], 0x3f800001U)) {
        return 1;
    }
    if (!has_bits(doubles[0], 0x3ff0000000000001UL) ||
        !has_bits(doubles[2], 0x3ff0000000000001UL)) {
        return 2;
    }
    if (!has_bits(singles[4], 0x80000000U) ||
        !has_bits(doubles[4], 0x8000000000000000UL)) {
        return 3;
    }
    if (singles[1] != 51.25F || singles[3] != 52.5F || singles[5] != 53.75F ||
        doubles[1] != 61.625 || doubles[3] != 62.875 || doubles[5] != 63.0625) {
        return 4;
    }
    return 0;
}

int check_integer_first() {
    unsigned long integers[5] = {};
    float singles[7] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 97.75F};
    double doubles[5] = {};
    exercise_integer_first(integers, singles, doubles);
    const unsigned long expected_integers[] = {
        0x1100000000000001UL, 0x2200000000000002UL, 0x3300000000000003UL,
        0x4400000000000004UL, 0x5500000000000005UL
    };
    const float expected_singles[] = {11.25F, 13.5F, 15.75F, 17.125F, 19.25F, 21.5F, 97.75F};
    const double expected_doubles[] = {12.625, 14.875, 16.0625, 18.3125, 20.5625};
    if (!matches(integers, expected_integers)) {
        return 11;
    }
    if (!matches(singles, expected_singles)) {
        return 12;
    }
    if (!matches(doubles, expected_doubles)) {
        return 13;
    }
    return 0;
}

int check_sse_first() {
    unsigned long integers[5] = {};
    float singles[6] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 98.125F};
    double doubles[5] = {};
    exercise_sse_first(integers, singles, doubles);
    const unsigned long expected_integers[] = {
        0x6600000000000006UL, 0x7700000000000007UL, 0x8800000000000008UL,
        0x9900000000000009UL, 0xaa0000000000000aUL
    };
    const float expected_singles[] = {32.5F, 34.75F, 36.125F, 38.25F, 40.5F, 98.125F};
    const double expected_doubles[] = {31.0625, 33.3125, 35.5625, 37.8125, 39.4375};
    if (!matches(integers, expected_integers)) {
        return 21;
    }
    if (!matches(singles, expected_singles)) {
        return 22;
    }
    if (!matches(doubles, expected_doubles)) {
        return 23;
    }
    return 0;
}

}

int main() {
    if (const int status = check_widths()) {
        return status;
    }
    if (const int status = check_integer_first()) {
        return status;
    }
    return check_sse_first();
}

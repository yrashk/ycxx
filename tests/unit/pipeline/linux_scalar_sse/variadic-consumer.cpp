#include <bit>
#include <climits>
#include <cstdarg>
#include <limits>

extern "C" int run_sse_variadics(float, float, const int*);

namespace {

static_assert(CHAR_BIT == 8 && sizeof(unsigned int) == 4 && sizeof(unsigned long long) == 8);
static_assert(sizeof(float) == 4 && sizeof(double) == 8);
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
static_assert(std::numeric_limits<unsigned short>::max() <= std::numeric_limits<int>::max());

constexpr int pointer_marker = 719;
constexpr double xmm_values[] = {1.25, 2.5, 3.75, 4.125, 5.5, 6.25, 7.875, 8.5, 9.25};

bool is_widened_input(double value) {
    return std::bit_cast<unsigned long long>(value) == 0x3ff0000020000000ULL;
}

bool is_negative_zero(double value) {
    return std::bit_cast<unsigned long long>(value) == 0x8000000000000000ULL;
}

}

extern "C" int check_named_and_promoted(float named_float, double named_double, ...) {
    std::va_list arguments;
    va_start(arguments, named_double);
    double promoted = va_arg(arguments, double);
    double zero = va_arg(arguments, double);
    double existing_double = va_arg(arguments, double);
    int signed_small = va_arg(arguments, int);
    int unsigned_small = va_arg(arguments, int);
    const int* pointer = va_arg(arguments, const int*);
    va_end(arguments);

    return std::bit_cast<unsigned int>(named_float) == 0x3f800001U && named_double == 2.25 &&
           is_widened_input(promoted) && is_negative_zero(zero) && existing_double == 91.875 &&
           signed_small == -27 && unsigned_small == 50021 && pointer == &pointer_marker;
}

extern "C" double check_empty_tail(double named, ...) {
    std::va_list arguments;
    va_start(arguments, named);
    va_end(arguments);
    return named == 11.25 ? -45.625 : 0.25;
}

extern "C" int check_empty_result(double returned) {
    return returned == -45.625;
}

extern "C" int check_xmm_count(int count, ...) {
    if (count != 7 && count != 8 && count != 9) {
        return 0;
    }

    std::va_list arguments;
    va_start(arguments, count);
    bool matches = true;
    for (int index = 0; index < count; ++index) {
        double actual = va_arg(arguments, double);
        if (actual != xmm_values[index]) {
            matches = false;
        }
    }
    va_end(arguments);
    return matches;
}

extern "C" int check_mixed_spills(int tag, ...) {
    std::va_list arguments;
    va_start(arguments, tag);
    bool matches = tag == 399;
    for (int index = 0; index < 8; ++index) {
        int integer = va_arg(arguments, int);
        double floating = va_arg(arguments, double);
        if (integer != 101 + index || floating != xmm_values[index]) {
            matches = false;
        }
    }
    int before_ninth = va_arg(arguments, int);
    double ninth = va_arg(arguments, double);
    int before_tenth = va_arg(arguments, int);
    double tenth = va_arg(arguments, double);
    const int* pointer = va_arg(arguments, const int*);
    double zero = va_arg(arguments, double);
    va_end(arguments);

    return matches && before_ninth == 109 && is_widened_input(ninth) &&
           before_tenth == 110 && tenth == 10.75 && pointer == &pointer_marker &&
           is_negative_zero(zero);
}

extern "C" int check_named_eight(double first, double second, double third, double fourth,
                                 double fifth, double sixth, double seventh, double eighth, ...) {
    std::va_list arguments;
    va_start(arguments, eighth);
    double promoted = va_arg(arguments, double);
    int signed_small = va_arg(arguments, int);
    double zero = va_arg(arguments, double);
    int unsigned_small = va_arg(arguments, int);
    const int* pointer = va_arg(arguments, const int*);
    va_end(arguments);

    return first == xmm_values[0] && second == xmm_values[1] && third == xmm_values[2] &&
           fourth == xmm_values[3] && fifth == xmm_values[4] && sixth == xmm_values[5] &&
           seventh == xmm_values[6] && eighth == xmm_values[7] && is_widened_input(promoted) &&
           signed_small == -27 && is_negative_zero(zero) && unsigned_small == 50021 &&
           pointer == &pointer_marker;
}

int main() {
    float value = std::bit_cast<float>(0x3f800001U);
    float negative_zero = std::bit_cast<float>(0x80000000U);
    return run_sse_variadics(value, negative_zero, &pointer_marker);
}

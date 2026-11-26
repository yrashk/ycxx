#include "empty_record_transport.hpp"

namespace {
int result_evaluations = 0;
bool result_arguments_valid = true;

bool first_six(long a, long b, long c, long d, long e, long f) {
    return a == 11 && b == 22 && c == 33 && d == 44 && e == 55 && f == 66;
}
}

bool six_integers(E, long a, long b, long c, E, long d, long e, long f, E) {
    return first_six(a, b, c, d, e, f);
}

bool stack_integers(E, long a, long b, long c, E, long d, long e, long f, E,
                    long seventh, E, long eighth, E) {
    return first_six(a, b, c, d, e, f) && seventh == -77 && eighth == 88;
}

E return_empty(E, long a, long b, long c, E, long d, long e, long f, E,
               long seventh, E, long eighth, E) {
    if (!first_six(a, b, c, d, e, f) || seventh != -77 || eighth != 88)
        result_arguments_valid = false;
    ++result_evaluations;
    return E{};
}

int return_operand_evaluations() {
    return result_evaluations;
}

bool return_arguments_correct() {
    return result_arguments_valid;
}

bool distinct_parameters(E first, E second, const E* original) {
    return compare_live_addresses(&first, &second, original);
}

Byte transfer_byte(Byte input, long left, long right) {
    if (input.value != 37 || left != 61 || right != 73)
        return Byte{0};
    return Byte{93};
}

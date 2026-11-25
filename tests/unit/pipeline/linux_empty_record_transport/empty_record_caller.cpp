#include "empty_record_transport.hpp"

namespace {
int argument_evaluations = 0;
int address_observations = 0;

E observe_argument() {
    ++argument_evaluations;
    return E{};
}
}

bool compare_live_addresses(const E* first, const E* second, const E* original) {
    ++address_observations;
    return first && second && original &&
           first != second && first != original && second != original;
}

int main() {
    if (!six_integers(observe_argument(), 11, 22, 33, E{}, 44, 55, 66, E{}))
        return 1;
    if (argument_evaluations != 1)
        return 2;
    if (!stack_integers(E{}, 11, 22, 33, E{}, 44, 55, 66, E{}, -77, E{}, 88, E{}))
        return 3;

    E original{};
    if (!distinct_parameters(original, original, &original))
        return 4;

    return_empty(E{}, 11, 22, 33, observe_argument(), 44, 55, 66, E{}, -77, E{}, 88, E{});
    if (argument_evaluations != 2 || return_operand_evaluations() != 1 ||
        !return_arguments_correct())
        return 5;

    E received = return_empty(E{}, 11, 22, 33, E{}, 44, 55, 66, E{}, -77, E{}, 88, observe_argument());
    if (argument_evaluations != 3 || return_operand_evaluations() != 2 ||
        !return_arguments_correct())
        return 6;
    if (!distinct_parameters(received, received, &received) || address_observations != 2)
        return 7;

    const Byte byte = transfer_byte(Byte{37}, 61, 73);
    return byte.value == 93 ? 0 : 8;
}

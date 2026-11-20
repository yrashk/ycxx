#include "scalar-copy.hpp"

Scalar main_adjust(Scalar input);

Scalar peer_adjust(Scalar input) {
    Scalar adjusted = main_adjust(input);
    return Scalar(adjusted.value + 5);
}

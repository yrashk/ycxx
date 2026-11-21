#include "scalar-copy.hpp"

Scalar::Scalar(int input) : value(input) {}

Scalar peer_adjust(Scalar input);

Scalar main_adjust(Scalar input) {
    return Scalar(input.value + 3);
}

int main() {
    const Scalar source(11);
    Scalar result = peer_adjust(source);
    return result.value == 19 && source.value == 11 ? 0 : 1;
}

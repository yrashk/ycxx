#include "volatile-copy.hpp"

Volatile main_adjust(Volatile input) {
    return Volatile(input.value + 3);
}

int main() {
    const Volatile source(11);
    const Volatile result = peer_adjust(source);
    return result.value == 19 && source.value == 11 ? 0 : 1;
}

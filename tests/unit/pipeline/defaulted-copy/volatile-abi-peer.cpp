#include "volatile-copy.hpp"

Volatile peer_adjust(Volatile input) {
    const Volatile adjusted = main_adjust(input);
    return Volatile(adjusted.value + 5);
}

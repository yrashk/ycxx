#include "calculate.hpp"

int peer_value();

int main() {
    if (calculate<40>(2) != 42)
        return 1;
    if (peer_value() != 47)
        return 2;
    return 0;
}

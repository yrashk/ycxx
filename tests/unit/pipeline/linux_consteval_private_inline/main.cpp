#include "steps.hpp"

int positive_runtime(decltype(sizeof(0)) size);
int negative_runtime(decltype(sizeof(0)) size);
Step peer_address();

int main() {
    volatile int input = 7;
    if (positive_runtime(input) != 41)
        return 1;
    if (negative_runtime(input) != 17)
        return 2;
    if (Steps::run(input) != 16)
        return 3;

    Step address = Steps::address();
    if (address != peer_address())
        return 4;
    if (address(input) != 14)
        return 5;
    return 0;
}

#include "hidden.hpp"

using Unary = int (*)(int);

Unary peer_step();
Unary peer_twice();

static_assert(hidden_twice(5) == 10);

int main() {
    Unary step = peer_step();
    Unary twice = peer_twice();

    if (step != &hidden_step)
        return 1;
    if (twice != &hidden_twice)
        return 2;
    if (step(6) != 7)
        return 3;
    if (twice(6) != 12)
        return 4;
    return 0;
}

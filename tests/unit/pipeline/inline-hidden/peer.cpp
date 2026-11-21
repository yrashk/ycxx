#include "hidden.hpp"

using Unary = int (*)(int);

Unary peer_step() {
    return &hidden_step;
}

Unary peer_twice() {
    return &hidden_twice;
}

#include "held.hpp"

Guard::~Guard() noexcept { ++*destroyed; }
Held hold(int* destroyed) noexcept { return Held{{destroyed}}; }

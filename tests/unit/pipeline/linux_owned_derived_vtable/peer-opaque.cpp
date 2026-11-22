#include "base.hpp"

int main() {
    if (make_and_dispatch(41) != 41) return 11;
    if (make_and_dispatch(-29) != -29) return 12;
}

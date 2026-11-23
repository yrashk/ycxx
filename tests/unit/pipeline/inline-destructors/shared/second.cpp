#include "owner.hpp"

int primary_destructions = 0;
int specialized_destructions = 0;

void destroy_second() {
    Owner owner;
}

int main() {
    destroy_first();
    if (primary_destructions != 1 || specialized_destructions != 1)
        return 1;
    destroy_second();
    if (primary_destructions != 2 || specialized_destructions != 2)
        return 2;
    return 0;
}

#include "owner.hpp"

int destructions = 0;

void destroy_second() {
    Owner owner;
}

int main() {
    destroy_first();
    if (destructions != 1)
        return 1;
    destroy_second();
    if (destructions != 2)
        return 2;
    return 0;
}

#ifdef DUPLICATE_DESTRUCTOR
Owner::~Owner() {
    ++destructions;
}
#endif

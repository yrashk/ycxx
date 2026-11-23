#include "owner.hpp"

void destroy_first() {
    Owner owner;
}

Owner::~Owner() {
    ++destructions;
}

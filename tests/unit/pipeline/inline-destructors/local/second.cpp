#include "owner.hpp"

namespace {
struct Tag {
    static constexpr int amount = 2;
};
}

int destructions = 0;

void destroy_second() {
    Owner<Tag> owner;
}

int main() {
    destroy_first();
    if (destructions != 1)
        return 1;
    destroy_second();
    if (destructions != 3)
        return 2;
    return 0;
}

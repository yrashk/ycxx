#include "owner.hpp"

namespace {
struct Tag {
    static constexpr int amount = 1;
};
}

void destroy_first() {
    Owner<Tag> owner;
}

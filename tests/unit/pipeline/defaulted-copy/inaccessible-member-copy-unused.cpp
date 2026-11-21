#include "inaccessible-member-copy.hpp"

int read_without_copy(int input) {
    const W original(input);
    return original.member.value;
}

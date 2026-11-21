#include "shared.hpp"

int assign_in_peer(int input) {
    const H source{{input}};
    H destination{{3}};
    H& result = (destination = source);
    return &result == &destination ? destination.member.value : -1;
}

#include "shared.hpp"

L& L::operator=(const L& source) {
    value = source.value + 7;
    return *this;
}

int main() {
    const H source{{13}};
    H destination{{2}};
    H& result = (destination = source);
    if (&result != &destination || destination.member.value != 20) {
        return 1;
    }
    return assign_in_peer(37) == 44 ? 0 : 2;
}

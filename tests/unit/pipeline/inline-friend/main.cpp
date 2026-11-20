#include "inline-address.hpp"

int main() {
    inline_address::Item item{29};
    inline_address::Reader local_address = &inline_address::read;
    return local_address == inline_address::peer_address() && local_address(item) == 33 ? 0 : 1;
}

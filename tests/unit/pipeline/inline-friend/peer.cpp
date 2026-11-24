#include "inline-address.hpp"

int inline_address::Item::bias() { return 0; }

inline_address::Reader inline_address::peer_address() {
    return &read;
}

int inline_template::peer_take() {
    Cell<int> cell{7};
    return take(cell);
}

int inline_address::peer_sum() {
    Item item{40};
    return item + 2;
}

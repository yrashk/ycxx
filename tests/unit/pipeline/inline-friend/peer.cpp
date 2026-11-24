#include "inline-address.hpp"

int inline_address::Item::bias() { return 0; }

inline_address::Reader inline_address::peer_address() {
    return &read;
}

static int local_size() {
    long p = 1, q = 2;
    auto f = [p, q] { return p + q; };
    return size_of(inline_template::Holder<decltype(f)>{f});
}

int inline_template::peer_local_size() {
    return local_size();
}

int inline_template::peer_take() {
    Cell<int> cell{7};
    return take(cell);
}

int inline_address::peer_sum() {
    Item item{40};
    return item + 2;
}

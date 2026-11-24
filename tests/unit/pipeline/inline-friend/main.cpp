#include "inline-address.hpp"

// The same closure type name as the peer's, of a different size.
static int local_size() {
    int x = 1;
    auto f = [x] { return x; };
    return size_of(inline_template::Holder<decltype(f)>{f});
}

int main() {
    inline_address::Item item{29};
    inline_address::Reader local_address = &inline_address::read;
    if (local_address != inline_address::peer_address() || local_address(item) != 33) return 1;
    if (item + 4 != 33 || item + 4L != -1 || operator+(item, 4) != 33 || inline_address::peer_sum() != 42) return 2;
    derived_operand::Item derived;
    nominated_operand::Item nominated;
    if (derived + 1 != 23 || nominated + 1 != 31) return 3;
    inline_template::Cell<int> cell{5};
    if (take(cell) != 6 || inline_template::peer_take() != 8) return 4;
    return local_size() == 4 && inline_template::peer_local_size() == 16 ? 0 : 5;
}

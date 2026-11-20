#ifndef HIDDEN_FRIEND_INLINE_ADDRESS_HPP
#define HIDDEN_FRIEND_INLINE_ADDRESS_HPP

namespace inline_address {
struct Item {
    int number;

    friend int read(const Item& item) {
        return item.number + 4;
    }
};

int read(const Item& item);

using Reader = int (*)(const Item&);
Reader peer_address();
}

#endif

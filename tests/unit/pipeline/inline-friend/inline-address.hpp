#ifndef HIDDEN_FRIEND_INLINE_ADDRESS_HPP
#define HIDDEN_FRIEND_INLINE_ADDRESS_HPP

namespace inline_address {
struct Item {
    int number;

    friend int read(const Item& item) {
        return item.number + 4;
    }

    friend int operator+(const Item& item, int amount) {
        return item.number + amount + bias();
    }

private:
    static int bias();
};

inline int operator+(const Item&, long) {
    return -1;
}

int read(const Item& item);

using Reader = int (*)(const Item&);
Reader peer_address();
int peer_sum();
}

namespace derived_operand { struct Item; }
namespace associated_base {
struct Base {};
inline int operator+(const derived_operand::Item&, int) { return 23; }
}
namespace derived_operand {
struct Item : associated_base::Base {
    friend int operator+(const Item&, long) { return -1; }
};
}

namespace nominated_operand { struct Item; }
namespace unrelated {
inline int operator+(const nominated_operand::Item&, int) { return -1; }
}
namespace nominated_operand {
using namespace unrelated;
struct Item {
    friend int operator+(const Item&, long) { return 31; }
};
}

#endif

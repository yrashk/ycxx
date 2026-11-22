#include "root.hpp"

int abi309::make_root() {
    Root root;
    root.count = 7;
    return peer_check(&root);
}

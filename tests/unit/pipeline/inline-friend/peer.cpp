#include "inline-address.hpp"

inline_address::Reader inline_address::peer_address() {
    return &read;
}

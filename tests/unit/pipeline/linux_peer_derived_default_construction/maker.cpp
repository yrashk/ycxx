#include "root.hpp"

extern "C" int make_derived(int initial, int updated) noexcept {
    D d;
    d.tag = initial;
    B& base = d;
    const int status = peer_observe(d, base, initial, updated);
    return status == 0 ? d.tag : status;
}

#include "root.hpp"

namespace {
using pure_probe::Root;

const void* seen_receiver;
int seen_argument;
int slot_calls;
const Root* receiver;
int receiver_calls;
int argument_calls;

int observed(const void* object, int value, int bias) noexcept {
    seen_receiver = object;
    seen_argument = value;
    ++slot_calls;
    return bias + value;
}

struct Primary : Root {
    int tag;

    int zeta(int value) const noexcept override { return observed(this, value, 100 + tag); }
    int alpha(int value) const noexcept override { return observed(this, value, 200 + tag); }
};

struct Anchor {
    virtual int marker(int) const noexcept { return -1; }
};

struct Secondary : Anchor, Root {
    int tag;

    int zeta(int value) const noexcept override { return observed(this, value, 300 + tag); }
    int alpha(int value) const noexcept override { return observed(this, value, 400 + tag); }
};

int verify(const Root& object, const void* complete, int zeta_bias, int alpha_bias) noexcept {
    slot_calls = 0;
    if (call_zeta(&object, 3) != zeta_bias + 3 || seen_receiver != complete ||
        seen_argument != 3 || slot_calls != 1)
        return 1;
    if (call_alpha(object, 5) != alpha_bias + 5 || seen_receiver != complete ||
        seen_argument != 5 || slot_calls != 2)
        return 2;

    receiver = &object;
    receiver_calls = 0;
    argument_calls = 0;
    if (pure_probe::call_with_effects() != alpha_bias + 7 || seen_receiver != complete ||
        seen_argument != 7 || slot_calls != 3)
        return 3;
    if (receiver_calls != 1 || argument_calls != 1)
        return 4;

    slot_calls = 0;
    receiver_calls = 0;
    argument_calls = 0;
    if (pure_probe::call_conditional(false) != 73 || slot_calls != 0 ||
        receiver_calls != 0 || argument_calls != 0)
        return 5;
    if (pure_probe::call_conditional(true) != alpha_bias + 7 || seen_receiver != complete ||
        seen_argument != 7 || slot_calls != 1 || receiver_calls != 1 || argument_calls != 1)
        return 6;
    return 0;
}
}

extern "C" const pure_probe::Root* peer_receiver() noexcept {
    ++receiver_calls;
    return receiver;
}

extern "C" int peer_argument() noexcept {
    ++argument_calls;
    return 7;
}

int main() {
    Primary primary;
    primary.tag = 13;
    if (const int status = verify(primary, &primary, 113, 213))
        return status;

    Secondary secondary;
    secondary.tag = 37;
    const Root& secondary_root = secondary;
    if (static_cast<const void*>(&secondary_root) == static_cast<const void*>(&secondary))
        return 8;
    if (const int status = verify(secondary_root, &secondary, 337, 437))
        return 16 + status;
    return 0;
}

#include "root.hpp"

namespace pure_probe {
int call_zeta(const Root* object, int value) noexcept {
    return object->zeta(value);
}

int call_alpha(const Root& object, int value) noexcept {
    return object.alpha(value);
}

int call_with_effects() noexcept {
    return peer_receiver()->alpha(peer_argument());
}

struct Result {
    int value;
};

int call_conditional(bool take_call) noexcept {
    Result result = take_call ? Result{peer_receiver()->alpha(peer_argument())} : Result{73};
    return result.value;
}
}

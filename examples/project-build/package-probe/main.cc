#include "codec_config.hh"

namespace codec = example::codec;

template<codec::support State>
constexpr int mode_status() {
    if constexpr (codec::available<State>) {
        return 0;
    } else if constexpr (State == codec::support::unsupported) {
        return 10;
    } else {
        return 20;
    }
}

int main() {
    return mode_status<codec::fast>();
}

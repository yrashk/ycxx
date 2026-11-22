#include "base.hpp"

namespace p {
Base::Base(int initial) noexcept : value(initial) {
    record_base_event(1, this);
}

Base::~Base() noexcept {
    record_base_event(4, this);
}
}

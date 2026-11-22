#ifndef ABI136_BASE_HPP
#define ABI136_BASE_HPP

namespace p {
struct Base {
    int value;
    Base(int initial) noexcept;
    ~Base() noexcept;
};
}

extern "C" void record_base_event(int phase, const p::Base* object) noexcept;

#endif

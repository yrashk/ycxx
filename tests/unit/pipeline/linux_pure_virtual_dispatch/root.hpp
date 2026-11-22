#ifndef ABI309_PURE_DISPATCH_HPP
#define ABI309_PURE_DISPATCH_HPP

namespace pure_probe {
struct Root {
    virtual int zeta(int value) const noexcept = 0;
    virtual int alpha(int value) const noexcept = 0;
};

int call_zeta(const Root* object, int value) noexcept;
int call_alpha(const Root& object, int value) noexcept;
int call_with_effects() noexcept;
int call_conditional(bool take_call) noexcept;
}

extern "C" const pure_probe::Root* peer_receiver() noexcept;
extern "C" int peer_argument() noexcept;

#endif

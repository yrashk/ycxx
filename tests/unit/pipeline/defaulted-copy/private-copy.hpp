#ifndef DEFAULTED_PRIVATE_COPY_HPP
#define DEFAULTED_PRIVATE_COPY_HPP

struct Copier;

class PrivateCopy {
    int value;
    constexpr PrivateCopy(const PrivateCopy&) noexcept = default;
    friend struct Copier;

public:
    PrivateCopy(int input) noexcept : value(input) {}
};

#endif

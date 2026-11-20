#ifndef DEFAULTED_SCALAR_COPY_HPP
#define DEFAULTED_SCALAR_COPY_HPP

struct Scalar {
    int value;

    Scalar(int input);
    constexpr Scalar(const Scalar&) noexcept = default;
};

#endif

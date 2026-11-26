#ifndef DEFAULTED_VOLATILE_COPY_HPP
#define DEFAULTED_VOLATILE_COPY_HPP

// [class.copy.ctor]: a volatile scalar does not make the copy nontrivial.
struct Volatile {
    volatile int value;

    Volatile(int input) : value(input) {}
    Volatile(const Volatile&) = default;
};

Volatile main_adjust(Volatile input);
Volatile peer_adjust(Volatile input);

#endif

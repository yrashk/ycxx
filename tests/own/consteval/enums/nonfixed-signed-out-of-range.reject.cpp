// XFAIL: gcc GCC 16 accepts an out-of-range enum cast in constant evaluation
enum SignedRange { signed_low = -2, signed_high = 1 };

[[maybe_unused]] constexpr SignedRange value = static_cast<SignedRange>(-0x100000001LL); //! error

// XFAIL: gcc GCC 16 accepts an out-of-range enum cast in constant evaluation
enum UnsignedRange { unsigned_low = 0, unsigned_named_high = 2 };

[[maybe_unused]] constexpr UnsignedRange value = static_cast<UnsignedRange>(0x100000003ULL); //! error

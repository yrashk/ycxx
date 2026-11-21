enum SignedRange { signed_low = -2, signed_high = 1 };

[[maybe_unused]] constexpr SignedRange value = static_cast<SignedRange>(-1LL);

static_assert(static_cast<int>(value) == -1);

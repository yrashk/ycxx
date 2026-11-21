enum UnsignedRange { unsigned_low = 0, unsigned_named_high = 2 };

[[maybe_unused]] constexpr UnsignedRange value = static_cast<UnsignedRange>(3ULL);

static_assert(static_cast<int>(value) == 3);

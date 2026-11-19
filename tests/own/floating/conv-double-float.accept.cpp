// [conv.double], [conv.fpprom]: a double converts to float exactly if representable, else to one of
// the two adjacent floats (the target rounds to nearest); float to double is exact.
static_assert(static_cast<float>(0.1) == 0.1f);
static_assert(static_cast<double>(0.1f) == 0x1.99999ap-4);
static_assert(static_cast<double>(static_cast<float>(0.1)) != 0.1);
static_assert(static_cast<float>(0.5) == 0.5f);
static_assert(static_cast<float>(1.0 + 0x1p-24) == 1.0f);   // a tie, to even
static_assert(static_cast<float>(1.0 + 0x1p-23) == 1.0f + 0x1p-23f);
static_assert(static_cast<float>(1.0 + 0x3p-25) == 1.0f + 0x1p-23f); // above the tie
static_assert(static_cast<float>(1e-50) == 0.0f);
static_assert(static_cast<float>(0x1p-149) == 0x1p-149f);
static_assert(static_cast<long double>(0.25) == 0.25L);
static_assert(static_cast<double>(0.25L) == 0.25);
constexpr float narrowed = 0.1; // an implicit conversion in copy-initialization
static_assert(narrowed == 0.1f);

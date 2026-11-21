constexpr unsigned high_bits = ~0u;

enum class Mask : unsigned { low = 3u, high = high_bits };

constexpr Mask high_from_signed = static_cast<Mask>(-1);
constexpr Mask below_high = static_cast<Mask>(high_bits - 1u);

static_assert(high_from_signed == Mask::high);
static_assert(below_high != Mask::high);
static_assert(static_cast<unsigned>(below_high) == high_bits - 1u);
static_assert(static_cast<unsigned long long>(high_from_signed) == high_bits);

int main() {
    Mask result = high_from_signed;
    return static_cast<unsigned long long>(result) == high_bits ? 0 : 1;
}

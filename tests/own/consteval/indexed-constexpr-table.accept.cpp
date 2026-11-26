namespace math {
enum class Constant : unsigned char { one, two };
struct Bits { int exp; unsigned long long significand; };
inline constexpr Bits table[] = { {17, 4u}, {-9, 8u} };
consteval int select(Constant c) {
    const Bits& b = math::table[static_cast<int>(c)];
    return b.exp;
}
}
static_assert(math::select(math::Constant::two) == -9);
static_assert(math::select(math::Constant::one) == 17);

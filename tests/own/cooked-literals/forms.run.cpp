// [over.literal]: both name spellings and canonical parameter types identify one literal operator.
// [lex.ext], [lex.icon]: bases, separators, range and precedence of ordinary integer literals.
// EXIT: 0
using U = unsigned long long;
constexpr U operator "" _k(const U x);
constexpr U operator""_k(U x) { return x; }
static_assert(0x2a_k == 42 && 0b10'1010_k == 42 && 052_k == 42 && 4'2_k == 42);
static_assert(18446744073709551615_k == ~0ULL && 9223372036854775808_k == 9223372036854775808ULL);
static_assert(0_k == 0 && 12LL == 12);
static_assert(operator""_k(23) == 23);
int main() { return 0; }

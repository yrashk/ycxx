// [lex.ext]: the cooked integer form takes precedence over visible raw and template forms.
// EXIT: 0
namespace R { constexpr int operator""_k(const char*) { return 3; } }
namespace T { template<char...> constexpr int operator""_k() { return 4; } }
namespace C { constexpr int operator""_k(unsigned long long) { return 2; } }
using namespace C;
using namespace R;
using namespace T;
static_assert(42_k == 2);
int main() { return 0; }

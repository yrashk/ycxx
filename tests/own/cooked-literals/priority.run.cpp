// [lex.ext]: a cooked integer operator determines the argument even with other visible forms.
// EXIT: 0
constexpr int operator""_k(unsigned long long) { return 2; }
int operator""_k(const char*);
int operator""_k(long double);
static_assert(0_k == 2);
int main() { return 0; }

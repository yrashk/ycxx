// [lex.ext], [over.literal]: a cooked user-defined integer literal calls the matching operator.
// EXIT: 0
constexpr unsigned long long operator""_k(unsigned long long v) { return v * 1000; }
int main() { return 2_k == 2000 ? 0 : 1; }

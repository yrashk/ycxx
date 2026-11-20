// [lex.ext], [expr.unary.op]: the literal is cooked before applying unary minus.
// EXIT: 0
constexpr int operator""_x(unsigned long long v) { return v ? 1 : 0; }
int main() { return -3_x == -1 ? 0 : 1; }

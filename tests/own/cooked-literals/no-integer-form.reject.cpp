// [lex.ext]: a floating parameter provides no applicable integer literal form.
constexpr int operator""_k(long double) { return 1; }
int f() { return 42_k; } //! error

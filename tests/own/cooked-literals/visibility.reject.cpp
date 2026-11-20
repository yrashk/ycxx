// [lex.ext], [basic.lookup.unqual]: a declaration after the literal is outside its lookup point.
int f() { return 1_k; } //! error
constexpr int operator""_k(unsigned long long) { return 2; }

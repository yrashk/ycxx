// [lex.ext], [basic.lookup.unqual]: the nearer declaration determines the visible set.
constexpr int operator""_k(unsigned long long) { return 1; }
namespace N {
constexpr int operator""_k(long double) { return 0; }
int f() { return 2_k; } //! error
}

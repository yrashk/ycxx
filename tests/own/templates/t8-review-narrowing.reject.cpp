// ζ t8 (#88), from the step's review: `__make_integer_seq`'s N is converted to T, even when it was
// dependent: 300 does not fit unsigned char ([temp.arg.nontype], docs/EXTENSIONS.md). Clang reports
// it at the alias, ycxx where it is used: one line.
// XFAIL: gcc GCC has no __make_integer_seq: it compiles the #else branch, which has no error
template<class T, T... Is> struct iseq { static constexpr int n = sizeof...(Is); };
#if __has_builtin(__make_integer_seq)
template<int N> using S = __make_integer_seq<iseq, unsigned char, N>; using bad = S<300>; //! error
#else
using bad = iseq<unsigned char>;
#endif
int main() { return 0; }

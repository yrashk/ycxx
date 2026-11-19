// ζ t8 (#88), from the step's review: `__make_integer_seq`'s N is converted to T, even when it was
// dependent: 300 does not fit unsigned char ([temp.arg.nontype], docs/EXTENSIONS.md). Clang reports
// it at the alias, ycxx where it is used: one line.
// XFAIL: gcc GCC has no __make_integer_seq
template<class T, T... Is> struct iseq { static constexpr int n = sizeof...(Is); };
template<int N> using S = __make_integer_seq<iseq, unsigned char, N>; using bad = S<300>; //! error
int main() { return 0; }

// ζ t8 (#88): `__make_integer_seq` with a negative length is ill-formed (docs/EXTENSIONS.md).
// XFAIL: gcc GCC has no __make_integer_seq: it compiles the #else branch, which has no error
template<class T, T... Is> struct iseq {};
#if __has_builtin(__make_integer_seq)
using bad = __make_integer_seq<iseq, int, -1>; //! error
#else
using bad = iseq<int>;
#endif
int main() { return 0; }

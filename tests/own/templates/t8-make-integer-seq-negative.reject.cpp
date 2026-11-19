// ζ t8 (#88): `__make_integer_seq` with a negative length is ill-formed (docs/EXTENSIONS.md).
template<class T, T... Is> struct iseq {};
using bad = __make_integer_seq<iseq, int, -1>; //! error
int main() { return 0; }

// #774: a scoped enumeration does not convert to an integral parameter, and an unscoped one does
// not narrow ([temp.arg.nontype]/1, [dcl.init.list]/7).
enum class K { a };
enum V : int { va = 300 };
template<int k> struct S {};
template<signed char k> struct C {};
S<K::a> s; //! error
C<va> c; //! error
int main() {}

// #774: an argument for a parameter of enumeration type is a converted constant expression of
// that type ([temp.arg.nontype]/1, [expr.const]); an integer does not convert to it.
enum class K { a, b };
enum U { u0 };
template<K k> struct S {};
template<U u> struct T {};
S<0> s; //! error
T<0> t; //! error
int main() {}

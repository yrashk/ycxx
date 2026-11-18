// ζ c2 (#88), from the step's review: a class template redeclared with a different
// template-head would be a second template of one name ([temp.pre]/7, [temp.over.link]). One
// line: Clang reports the second declaration, GCC the first.
template<class T> struct B; template<int N> struct B { }; //! error
int main() { return 0; }

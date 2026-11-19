// ζ s7 (#88): a static data member of a class template specialization may be explicitly
// specialized; it is then defined by that declaration ([temp.expl.spec]).
// EXIT: 0
template<class T = int> struct A { static int x; };
template<> int A<char>::x = 3;
int main() { A<char>::x += 1; return A<char>::x == 4 ? 0 : 1; }

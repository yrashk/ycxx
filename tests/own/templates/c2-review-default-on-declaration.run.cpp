// ζ c2 (#88), from the step's review: a default template argument given on an earlier
// declaration of the class template applies ([temp.param]/12).
// EXIT: 0
template<class T, class U = int> struct B;
template<class T, class U> struct B { T t; U u; };
int main() { B<char> b{1, 2}; return b.u - 2; }

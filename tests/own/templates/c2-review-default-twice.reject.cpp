// ζ c2 (#88), from the step's review: a default template argument is given by one
// declaration only ([temp.param]/12).
template<class T, class U = int> struct B;
template<class T, class U = int> struct B { T t; U u; }; //! error
int main() { return 0; }

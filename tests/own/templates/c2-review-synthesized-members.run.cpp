// ζ c2 (#88), from the step's review: a destructor that a function template's
// instance makes a class need is instantiated too ([temp.inst]/4, [class.dtor]).
// EXIT: 0
// STDOUT: C
extern "C" int putchar(int);
template<class T> struct D { T v; ~D() { putchar(v); putchar('\n'); } };
template<class T> struct H { D<T> d; };
template<class T> int make() { H<T> h{{67}}; return 1; }
int main() { return make<int>() - 1; }

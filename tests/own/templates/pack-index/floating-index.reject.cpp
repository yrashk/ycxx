// #297: the index is a converted constant expression of type std::size_t ([dcl.type.pack.index]/2),
// which allows no floating-integral conversion ([expr.const]).
// XFAIL: clang Clang 23 adds a second error after the index ("expected a type")
template<class... T> int f() { using X = T...[1.5]; return 0; } //! error@+0:47
int main() { return f<int, char>(); }

// ζ c6 (#88), from the step's review: a trailing requires-clause may name the function's
// parameters ([dcl.decl]/4, [basic.scope.param]).
// EXIT: 0
template<class T> int f(T t) requires (sizeof(t) > 2) { return 1; }
template<class T> int f(T t) { return 2; }
template<class T> int g(T* p) requires requires { *p; } { return 1; }
int main() { int x = 0; return f(1L) == 1 && f((char)1) == 2 && g(&x) == 1 ? 0 : 1; }

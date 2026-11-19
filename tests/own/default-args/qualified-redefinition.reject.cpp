// ι (#300), from the review:  a qualified definition may not repeat a default argument ([dcl.fct.default]/4).
namespace N { int f(int = 1); }
int N::f(int x = 2) { return x; } //! error
int main() { return N::f(); }

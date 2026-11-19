// ι (#300), from the review:  a member function or constructor with a parameter without a default after one with
// one ([dcl.fct.default]/4).
struct S { int f(int a = 1, int b) { return a + b; } }; //! error
struct T { T(int a = 1, int b) {} }; //! error
int main() { return 0; }

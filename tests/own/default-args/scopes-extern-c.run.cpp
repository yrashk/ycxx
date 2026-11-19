// ι (#300), from the review:  declarations in different scopes have their own default arguments
// ([dcl.fct.default]/4), even of one extern "C" function.
// EXIT: 12
namespace A { extern "C" int f(int x = 1); }
namespace B { extern "C" int f(int x = 2); }
extern "C" int f(int x) { return x; }
int main() { return A::f() * 10 + B::f(); }

// #133: a using-declaration introduces the declarations qualified lookup finds where it is; an
// overload declared later is not introduced ([namespace.udecl]/1, /11).
// EXIT: 0
namespace N { int f(int) { return 1; } }
using N::f;
namespace N { int f(long) { return 2; } }
int main() { return f(1L) - 1; }

// A matching earlier declaration supplies C language linkage ([dcl.link], #455).
namespace N {
extern "C" int f();
int f() { return 42; }
}
int main() { return N::f() == 42 ? 0 : 1; }

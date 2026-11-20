// Redeclarations preserve the corresponding function's linkage and type ([dcl.link], [basic.link], #455).
namespace N {
int f(int x) { return x; }
extern "C" int f(int); //! error
}

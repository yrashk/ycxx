// Redeclarations preserve the corresponding function's linkage and type ([dcl.link], [basic.link], #455).
namespace N {
extern "C" int f(int);
long f(int x) { return x; } //! error
}

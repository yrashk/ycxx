// #133: a using-declaration's target conflicts with a variable of the scope ([namespace.udecl]/10).
int x;
namespace N { int x; }
using N::x; //! error

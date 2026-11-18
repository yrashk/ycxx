// #133: a declaration conflicts with an earlier using-declaration's target ([basic.scope.scope]/4).
namespace N { int x; }
using N::x;
int x; //! error

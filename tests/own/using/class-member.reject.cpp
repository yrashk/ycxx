// #133: a class member's using-declaration is a member-declaration ([namespace.udecl]/8).
struct C { static int m; };
using C::m; //! error

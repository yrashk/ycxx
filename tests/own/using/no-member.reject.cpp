// #133: a using-declaration must name a declaration ([namespace.udecl]/1).
namespace N {}
using N::nothing; //! error

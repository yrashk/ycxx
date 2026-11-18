// #133: a using-declaration names no namespace ([namespace.udecl]/3).
namespace N {}
using ::N; //! error

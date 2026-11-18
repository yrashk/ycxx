// #133, from the step's review: two declarators of one using-declaration whose targets conflict
// ([namespace.udecl]/10).
namespace A { int x; }
namespace B { int x; }
using A::x, B::x; //! error

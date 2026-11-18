// #133: two using-declarations of different variables of one name ([namespace.udecl]/10).
namespace A { int x; }
namespace B { int x; }
using A::x;
using B::x; //! error

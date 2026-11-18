// δ s4 (#86), from the namespaces' review: a block using-directive's names appear in the
// namespace enclosing both it and the nominated one ([namespace.udir]/2); using-directives and
// using-declarations of one block make one overload set ([over.match]); a block's
// using-declaration hides an enclosing block's local ([basic.scope.hiding]); qualified lookup
// follows a namespace's using-directives ([namespace.qual]/2), and they are transitive
// ([namespace.udir]/4); a namespace's own `main` is not the program's ([basic.start.main]); a
// variable with C language linkage is one variable in every namespace ([dcl.link]); a qualified
// type as the first parameter.
// EXIT: 0
namespace M { int x = 1; }
namespace N2 { int x = 2; int f() { using namespace M; return x; } }
namespace A { int f(int) { return 1; } int v = 1; }
namespace B { int f(long) { return 2; } }
namespace P { int y = 3; }
namespace Q { using namespace P; }
namespace R { using namespace Q; }
namespace S { int main() { return 7; } }
namespace C1 { extern "C" int cvar; }
namespace C2 { extern "C" int cvar; }
extern "C" int cvar = 5;
namespace T { struct Box { int v; }; }
int h(T::Box* b) { return b->v; }

int main() {
  if (N2::f() != 2) return 1;
  {
    using A::f;
    using B::f;
    if (f(1) != 1 || f(1L) != 2) return 2;
  }
  {
    using namespace A;
    using namespace B;
    if (f(1) * 10 + f(1L) != 12) return 3;
  }
  int v = 2;
  {
    using A::v;
    if (v != 1) return 4;
  }
  if (v != 2) return 5;
  if (Q::y != 3 || R::y != 3) return 6;
  if (S::main() != 7) return 7;
  if (C1::cvar + C2::cvar != 10) return 8;
  T::Box b{9};
  if (h(&b) != 9) return 9;
  return 0;
}

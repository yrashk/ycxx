// [basic.lookup.qual], [namespace.udir]
namespace A { namespace N { using T = int; } }
namespace B { namespace N { using T = int; } }
namespace C { using namespace A; using namespace B; }
int check() {
  return typename C::N::T(1); //! error
}

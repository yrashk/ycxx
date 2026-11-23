// [basic.lookup.general], [dcl.typedef]
namespace A { using T = int; }
namespace B { using T = long; }
namespace C { using namespace A; using namespace B; }
int check() {
  return typename C::T(13); //! error
}

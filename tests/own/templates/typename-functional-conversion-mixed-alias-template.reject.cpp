// [basic.lookup.general], [namespace.udir], [temp.alias]
namespace A { using T = int; }
namespace B { template<class> using T = int; }
namespace C { using namespace A; using namespace B; }
int check() {
  return typename C::T(1); //! error
}

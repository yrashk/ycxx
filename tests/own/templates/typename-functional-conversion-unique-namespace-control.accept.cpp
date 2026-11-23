// [basic.lookup.qual], [namespace.udir]
namespace A { namespace N { using T = int; } }
namespace B { namespace M { using T = long; } }
namespace C { using namespace A; using namespace B; }
int main() {
  return typename C::N::T(13) != 13;
}

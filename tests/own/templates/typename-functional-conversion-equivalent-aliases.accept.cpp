// [basic.lookup.general], [dcl.typedef]
namespace A { using T = int; }
namespace B { using T = int; }
namespace C { using namespace A; using namespace B; }
int main() {
  return typename C::T(13) != 13;
}

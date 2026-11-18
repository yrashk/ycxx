// δ s4 (#86): a name not declared in the namespace named, a namespace member used without
// qualification, and a name made ambiguous by two using-directives
// ([basic.lookup.qual], [namespace.udir], [basic.lookup.general]).
namespace a {
  int f() { return 1; }
  int shared = 1;
}

namespace b {
  int g() { return 2; }
  int shared = 2;
}

using namespace a;
using namespace b;

int main() {
  int x = a::g(); //! error
  int y = b::nothing; //! error
  int z = shared; //! error
  int ok = f() + g() + a::shared + b::shared;
  return x + y + z + ok;
}

namespace c {
  int h() { return 3; }
}

int later() {
  return h(); //! error
}

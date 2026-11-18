// δ s4 (#86): `using namespace` directives at namespace and block scope, `using a::f;`
// declarations, namespace aliases, inline namespaces, and unnamed namespaces
// ([namespace.udir], [namespace.udecl], [namespace.alias], [namespace.unnamed],
// [namespace.def]).
// EXIT: 0
namespace math {
  int square(int v) { return v * v; }
  int cube(int v) { return v * v * v; }
  namespace detail {
    int twice(int v) { return 2 * v; }
  }
}

namespace text {
  int length(const char* s) {
    int n = 0;
    while (s[n] != 0) n++;
    return n;
  }
}

namespace lib {
  inline namespace v2 {
    int version() { return 2; }
  }
  namespace v1 {
    int version() { return 1; }
  }
}

namespace {
  int hidden = 42;
  int helper() { return hidden + 1; }
}

namespace md = math::detail;

using namespace text;

int main() {
  if (length("four") != 4) return 1;  // via the directive at namespace scope
  if (md::twice(21) != 42) return 2;
  using math::square;
  if (square(5) != 25) return 3;
  {
    using namespace math;
    if (cube(2) != 8) return 4;
  }
  if (hidden != 42 || helper() != 43) return 5;
  if (lib::version() != 2) return 6;
  if (lib::v1::version() != 1 || lib::v2::version() != 2) return 7;
  namespace d = md;
  if (d::twice(1) != 2) return 8;
  return 0;
}

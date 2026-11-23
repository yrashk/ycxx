// #953 [namespace.udecl], [basic.scope.scope]
namespace N {
  struct X { static constexpr int n = 1; };
  using T = X;
}
struct Y { static constexpr int n = 2; };

int f() {
  using N::T;
  using T = Y; //! error
  return T::n;
}

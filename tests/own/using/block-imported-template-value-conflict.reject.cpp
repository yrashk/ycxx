// #953 [namespace.udecl], [basic.scope.scope]
namespace N { template<class> struct T {}; }

void f() {
  using N::T;
  int T = 1; //! error
}

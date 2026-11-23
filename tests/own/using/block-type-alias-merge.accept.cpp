// #953 [namespace.udecl], [dcl.typedef], [basic.scope.scope]
namespace N {
  struct X { static constexpr int n = 1; };
  using T = X;
}
struct Y { static constexpr int n = 2; };

void alias_first() {
  using T = N::X;
  using N::T;
  static_assert(T::n == 1);
}

void import_first() {
  using N::T;
  using T = N::X;
  static_assert(T::n == 1);
}

void nested_alias() {
  using N::T;
  {
    using T = Y;
    static_assert(T::n == 2);
  }
  static_assert(T::n == 1);
}

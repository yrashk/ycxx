// #953 [namespace.udecl], [basic.scope.scope]
namespace N {
  struct Box { static constexpr int value = 17; };
  using E = Box;
}

int f() {
  using N::E;
  int E = 0; //! error
  return E::value;
}

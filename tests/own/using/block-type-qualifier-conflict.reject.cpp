// #953 [namespace.udecl]
namespace A { enum class E { one = 1 }; }
namespace B { enum class E { one = 2 }; }

int f() {
  using A::E;
  using B::E; //! error
  return static_cast<int>(E::one);
}

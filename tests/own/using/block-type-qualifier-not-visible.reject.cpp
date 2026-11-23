// #953 [namespace.udecl], [basic.lookup.qual]
namespace Imported { enum class E { one = 1 }; }

int outside() {
  {
    using Imported::E;
    static_assert(static_cast<int>(E::one) == 1);
  }
  return static_cast<int>(E::one); //! error
}

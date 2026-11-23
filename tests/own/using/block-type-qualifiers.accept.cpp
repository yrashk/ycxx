// #953 [namespace.udecl], [basic.lookup.qual]
namespace Imported {
  enum class E { one = 11 };
  struct Box { static constexpr int value = 17; };
  using Alias = Box;
}

void direct_imports() {
  using Imported::E;
  using Imported::Box;
  using Imported::Alias;
  static_assert(static_cast<int>(E::one) == 11);
  static_assert(Box::value == 17);
  static_assert(Alias::value == 17);
  using Imported::E;
  static_assert(static_cast<int>(E::one) == 11);
}

int tag_and_local_value() {
  using Imported::Box;
  int Box = 3;
  return Box::value + Box;
}

int alias_and_nested_value() {
  using Imported::Alias;
  {
    int Alias = 5;
    return Alias::value + Alias;
  }
}

namespace Inner { enum class E { one = 29 }; }

namespace Enclosing {
  enum class E { one = 23 };

  void nested_imports() {
    {
      using Imported::E;
      static_assert(static_cast<int>(E::one) == 11);
      {
        using Inner::E;
        static_assert(static_cast<int>(E::one) == 29);
      }
      static_assert(static_cast<int>(E::one) == 11);
    }
    static_assert(static_cast<int>(E::one) == 23);
  }
}

// [basic.lookup.udir] restricts the directive and alias to namespace names.
namespace Shadowed { constexpr int namespace_value = 41; }
namespace Types { enum class Shadowed { enumerator = 43 }; }

void namespace_only() {
  using Types::Shadowed;
  using namespace Shadowed;
  static_assert(namespace_value == 41);
  namespace Original = Shadowed;
  static_assert(Original::namespace_value == 41);
  static_assert(static_cast<int>(Shadowed::enumerator) == 43);
}

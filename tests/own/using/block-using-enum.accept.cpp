// Unqualified `using enum` resolves enumerations introduced by block-scope using-declarations
// ([enum.udecl], [namespace.udecl], [basic.lookup.general], [basic.scope.block], #1695).
namespace Library {
  enum class Kind { Small = 4 };
  enum class AbsentOuter { Only = 5 };
  enum class Empty : int {};
  enum Unscoped { Red = 9 };
}
namespace OtherNs {
  enum class Kind { Large = 12 };
}
namespace AliasNs {
  enum class Real { Aliased = 6 };
  using Kind = Real;
}
namespace InlineWrap {
  inline namespace Inner {
    enum class Kind { Inlined = 15 };
  }
}

enum class Kind { Other = 7 };

// Competing outer enum is hidden by block `using Library::Kind;`.
constexpr int width() {
  using Library::Kind;
  using enum Kind;
  return static_cast<int>(Small);
}
static_assert(width() == 4);

// Absent outer name resolves through block `using`.
constexpr int absent_outer() {
  using Library::AbsentOuter;
  using enum AbsentOuter;
  return static_cast<int>(Only);
}
static_assert(absent_outer() == 5);

// Imported namespace type alias to an enum.
constexpr int via_alias() {
  using AliasNs::Kind;
  using enum Kind;
  return static_cast<int>(Aliased);
}
static_assert(via_alias() == 6);

// Imported through an inline namespace.
constexpr int via_inline_ns() {
  using InlineWrap::Kind;
  using enum Kind;
  return static_cast<int>(Inlined);
}
static_assert(via_inline_ns() == 15);

// Imported unscoped enumeration.
constexpr int unscoped_import() {
  using Library::Unscoped;
  using enum Unscoped;
  return Red;
}
static_assert(unscoped_import() == 9);

// Complete empty enumeration is valid in `using enum`.
constexpr int empty_enum() {
  using Library::Empty;
  using enum Empty;
  return 1;
}
static_assert(empty_enum() == 1);

// Nested block shadowing and scope exit.
constexpr int nested_shadow_and_exit() {
  int total = 0;
  {
    using Library::Kind;
    {
      using enum Kind;
      total += static_cast<int>(Small);  // 4
    }
    {
      using OtherNs::Kind;
      using enum Kind;
      total += static_cast<int>(Large);  // 12
    }
    // Back in Library::Kind scope after inner block exit.
    using enum Kind;
    total += static_cast<int>(Small);    // 4
  }
  // Back in global ::Kind scope after outer block exit.
  using enum Kind;
  total += static_cast<int>(Other);      // 7
  return total;
}
static_assert(nested_shadow_and_exit() == 27);

// Inside a class member function, block `using` hides the member `enum class Kind`.
struct Holder {
  enum class Kind { Member = 20 };
  static constexpr int run() {
    using Library::Kind;
    using enum Kind;
    return static_cast<int>(Small);
  }
};
static_assert(Holder::run() == 4);

// Inside a function template, block `using` shadows a template type parameter.
template<class Kind>
constexpr int shadow_tparam() {
  using Library::Kind;
  using enum Kind;
  return static_cast<int>(Small);
}
static_assert(shadow_tparam<int>() == 4);

// Idempotent block `using` and compatible alias.
constexpr int idempotent() {
  using Library::Kind;
  using Library::Kind;
  using Kind = Library::Kind;
  using enum Kind;
  return static_cast<int>(Small);
}
static_assert(idempotent() == 4);

// Local class inside a block with `using Library::Kind;`.
constexpr int in_local_class() {
  using Library::Kind;
  struct Local {
    static constexpr int get() {
      using enum Kind;
      return static_cast<int>(Small);
    }
  };
  return Local::get();
}
static_assert(in_local_class() == 4);

// Non-generic and generic lambdas inside a block with `using Library::Kind;`.
constexpr int in_lambdas() {
  using Library::Kind;
  auto nongeneric = []() {
    using enum Kind;
    return static_cast<int>(Small);
  };
  auto generic = [](auto x) {
    using enum Kind;
    return static_cast<int>(Small) + x;
  };
  return nongeneric() + generic(3);
}
static_assert(in_lambdas() == 11);

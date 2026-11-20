// [class.ctor.general], [dcl.constexpr]: constructor specifiers must agree.
struct LostConstexpr {
  constexpr LostConstexpr();
};

LostConstexpr::LostConstexpr() {}  //! error

struct AddedConstexpr {
  AddedConstexpr();
};

constexpr AddedConstexpr::AddedConstexpr() {}  //! error

struct StaticConstructor {
  static constexpr StaticConstructor() {}  //! error
};

struct QualifiedConstructor {
  constexpr QualifiedConstructor() const {}  //! error
};

struct OutRef { constexpr OutRef(); };
constexpr OutRef::OutRef() & {}  //! error

struct OutReturn { constexpr OutReturn(); };
constexpr OutReturn::OutReturn() -> void {}  //! error

struct InlineReturn { constexpr InlineReturn() -> void {} };  //! error
struct InlineRef { constexpr InlineRef() noexcept & {} };  //! error

namespace A { struct Misplaced { constexpr Misplaced(); }; }
namespace B { constexpr A::Misplaced::Misplaced() {} }  //! error
